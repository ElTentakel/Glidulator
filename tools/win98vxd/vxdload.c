/* Load the dynamic VxD glsmp.vxd, exercise it, print results.
 * Win9x dynamic VxD: CreateFile("\\.\glsmp.vxd", FILE_FLAG_DELETE_ON_CLOSE)
 * loads it; closing the handle unloads it. Output to VXDLOAD.TXT.
 *
 *   DIOC 1 (ECHO)  : VxD writes CAFEBABE (smoke test)
 *   DIOC 2 (ALLOC) : VxD calls _PageAllocate(PageFixed|Contig) and returns
 *                    {handle, ring-0 linear, physical, pages}
 *   DIOC 3 (FREE)  : VxD frees the block
 *   DIOC 4 (START) : VxD starts the other cores (APs) by INIT/SIPI and runs
 *                    ap_pm32 on them; returns {alive, running, heartbeat,
 *                    tramp phys, APIC linear, status}
 *   DIOC 5 (STATUS): returns {alive, running, heartbeat, parked, MSR 1Bh} now
 *   DIOC 6 (STOP)  : INIT the APs back to wait-for-SIPI, free the trampoline
 *   DIOC 7 (DELAY) : input {ms}: VxD runs delay_ms, timed here with QPC
 *
 * "VXDLOAD nostop" skips STOP and calls FREE while the APs still run (the VxD
 * must stop them itself first); log VXDNOST.TXT instead.
 * "VXDLOAD x2apic|notbsp|hi4g" pass a test XOR for the MSR 1Bh value that
 * START checks (sets bit 10 / clears bit 8 / sets a base bit above 4 GB);
 * START must refuse with status 6/7/8 and start nothing. Logs VXDX2A.TXT,
 * VXDNBSP.TXT, VXDHI4G.TXT.
 * "VXDLOAD race" runs a second thread that keeps calling STATUS while the main
 * thread runs the normal sequence; calls that overlap a state-changing call
 * must fail with ERROR_BUSY, never crash. Log VXDRACE.TXT.
 *
 * Jobs on the APs (plan (c), step c1):
 *   DIOC 8 (LOCK)  : input {linear, bytes}: _LinPageLock in this process
 *   DIOC 9 (UNLOCK): unlock all locked ranges
 *   DIOC 10 (RUN)  : input {ap, fn, arg, fpu cw, mxcsr}: the AP calls fn(arg)
 *   DIOC 11 (JOB)  : input {ap}: {state, ret, vector, eip, cr2}
 * "VXDLOAD run": every AP runs work() from this EXE on its own buffer in this
 * process's private memory (EXE image and buffers locked before START); this
 * thread watches the round counters, stops the jobs and checks the buffers
 * and an x87 result bit for bit; then a job that page-faults on purpose must
 * come back as a fault (vector 14). Log VXDRUN.TXT.
 * "VXDLOAD switch": the same, but while the jobs run a child process
 * ("VXDLOAD child") allocates and touches most of the physical memory, so
 * Windows switches process contexts and pages. Log VXDSW.TXT.
 * "VXDLOAD hold": the jobs run for 60 s with a log line every 5 s, so the
 * process can be killed meanwhile (Ctrl+Alt+Del, End Task): the VxD must
 * stop the APs before Windows frees the process memory. Log VXDHOLD.TXT.
 * "VXDLOAD observe": opens a second handle first and logs STATUS changes for
 * 90 s (start it before "hold"): shows whether the kill stopped the APs and
 * whether the locks were released. Log VXDOBS.TXT. */
#define _WIN32_WINNT 0x0400
#include <windows.h>
#include <stdio.h>
#include <string.h>
static const char *g_log = "VXDLOAD.TXT";

/* race mode: second thread hammering STATUS */
static HANDLE g_h;
static volatile LONG g_race_stop;
static volatile LONG g_race_ok, g_race_busy, g_race_other;
static volatile DWORD g_race_other_err;
static DWORD WINAPI race_thread(LPVOID unused){
  (void)unused;
  while (!g_race_stop){
    DWORD st[5], r = 0;
    if (DeviceIoControl(g_h, 5, NULL,0, st,sizeof st, &r, NULL))
      InterlockedIncrement((LONG *)&g_race_ok);
    else if (GetLastError() == ERROR_BUSY)
      InterlockedIncrement((LONG *)&g_race_busy);
    else {
      InterlockedIncrement((LONG *)&g_race_other);
      g_race_other_err = GetLastError();
    }
  }
  return 0;
}
static void L(const char *fmt, ...){ va_list a; va_start(a,fmt);
  FILE *f=fopen(g_log,"a"); if(f){vfprintf(f,fmt,a);fclose(f);} va_end(a); }

static const char *smp_err(DWORD s);

/* ---- c1: jobs on the APs ---- */
struct job {
  volatile LONG stop;         /* set by this thread, polled by the AP */
  volatile DWORD rounds;      /* completed rounds */
  volatile DWORD errors;      /* words the AP read back wrong */
  DWORD ap;
  DWORD *buf;
  DWORD words;
  volatile DWORD last_seed;   /* seed of the last completed round */
  volatile double x87;        /* x87 result, checked bit for bit */
};
#define JOB_WORDS (256 * 1024 / 4)
#define JOB_SPAN  (JOB_WORDS * 4 + 4096)  /* buffer + its struct job page */

static double x87_series(void){
  volatile double x = 1.0;
  for (int k = 0; k < 1000; k++) x = x * 1.0000001 + 0.5;
  return x;
}
/* Runs on an AP in ring 0: no CRT, no API, no FS. */
static __attribute__((noinline)) int __cdecl work(struct job *j){
  DWORD r = 0;
  j->x87 = x87_series();
  while (!j->stop){
    DWORD *b = j->buf, n = j->words, seed = r * 0x9E3779B9u + j->ap, bad = 0;
    for (DWORD i = 0; i < n; i++) b[i] = seed ^ (i * 2654435761u);
    for (DWORD i = 0; i < n; i++) if (b[i] != (seed ^ (i * 2654435761u))) bad++;
    j->errors += bad;
    j->last_seed = seed;
    j->rounds = ++r;
  }
  return (int)r;
}
static __attribute__((noinline)) int __cdecl fault_fn(void *unused){
  (void)unused;
  return (int)*(volatile DWORD *)0x7FFF0000;   /* no PDE there: page fault */
}

static int run_child(void){
  MEMORYSTATUS ms; ms.dwLength = sizeof ms;
  GlobalMemoryStatus(&ms);
  SIZE_T sz = ms.dwTotalPhys / 10 * 8;
  BYTE *p = VirtualAlloc(NULL, sz, MEM_COMMIT, PAGE_READWRITE);
  if (!p) return 1;
  for (int pass = 0; pass < 2; pass++)
    for (SIZE_T i = 0; i < sz; i += 4096) p[i] = (BYTE)(i >> 12) + pass;
  Sleep(500);
  VirtualFree(p, 0, MEM_RELEASE);
  return 0;
}

static BOOL vxd_lock(HANDLE h, const void *p, DWORD bytes){
  DWORD in[2] = { (DWORD)p, bytes }, st = 99, r = 0;
  BOOL ok = DeviceIoControl(h, 8, in,sizeof in, &st,sizeof st, &r, NULL) && st == 0;
  L("LOCK : %08lX + %lu KB -> %s (status %lu)\r\n", (unsigned long)in[0],
    (unsigned long)(bytes / 1024), ok ? "locked" : "FAILED", (unsigned long)st);
  return ok;
}
static DWORD vxd_run(HANDLE h, DWORD ap, int (__cdecl *fn)(), void *arg){
  WORD cw; __asm__ volatile ("fnstcw %0" : "=m"(cw));
  DWORD in[5] = { ap, (DWORD)fn, (DWORD)arg, cw, 0x1F80 }, st = 99, r = 0;
  if (!DeviceIoControl(h, 10, in,sizeof in, &st,sizeof st, &r, NULL)) st = 98;
  return st;
}
static void vxd_job(HANDLE h, DWORD ap, DWORD out[5]){
  DWORD r = 0;
  memset(out, 0, 5 * sizeof out[0]);
  DeviceIoControl(h, 11, &ap,sizeof ap, out,5 * sizeof out[0], &r, NULL);
}
static const char *job_state(DWORD s){
  static const char *n[] = { "idle", "posted", "running", "done", "FAULT" };
  return s < 5 ? n[s] : "?";
}

static int run_jobs(HANDLE h, int sw, int hold){
  DWORD ret = 0;
  BYTE *base = (BYTE *)GetModuleHandleA(NULL);
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  L("EXE  : image %p, %lu KB; work=%p fault_fn=%p\r\n", base,
    (unsigned long)(nt->OptionalHeader.SizeOfImage / 1024), work, fault_fn);
  BYTE *mem = VirtualAlloc(NULL, 8 * JOB_SPAN, MEM_COMMIT, PAGE_READWRITE);
  if (!mem){ L("VirtualAlloc failed\r\n"); return 1; }
  memset(mem, 0, 8 * JOB_SPAN);
  /* everything the APs touch, locked before START (START copies the PDEs) */
  if (!vxd_lock(h, base, nt->OptionalHeader.SizeOfImage) ||
      !vxd_lock(h, mem, 8 * JOB_SPAN))
    return 1;

  DWORD info[4] = {0,0,0,0}, s[10] = {0}, x[2] = {0,0};
  DeviceIoControl(h, 2, NULL,0, info,sizeof info, &ret, NULL);
  DeviceIoControl(h, 4, x,sizeof x, s,sizeof s, &ret, NULL);
  L("START: status=%lu (%s), running=%lu, usable=%lu (HT siblings %lu)\r\n",
    (unsigned long)s[5], smp_err(s[5]), (unsigned long)s[1], (unsigned long)s[6],
    (unsigned long)s[7]);
  DWORD n = s[6] > 8 ? 8 : s[6];   /* job slots: one AP per physical core */
  if (s[5] != 0 || n == 0) return 1;

  struct job *jb[8];
  for (DWORD a = 0; a < n; a++){
    jb[a] = (struct job *)(mem + a * JOB_SPAN);
    jb[a]->ap = a;
    jb[a]->buf = (DWORD *)(mem + a * JOB_SPAN + 4096);
    jb[a]->words = JOB_WORDS;
    DWORD st = vxd_run(h, a, (int (__cdecl *)())work, jb[a]);
    L("RUN  : AP %lu work() -> %s\r\n", (unsigned long)a, st ? "REFUSED" : "posted");
  }

  /* watch: every AP's round counter must keep rising */
  HANDLE child = NULL;
  if (sw){
    char path[MAX_PATH], cmd[MAX_PATH + 8];
    GetModuleFileNameA(NULL, path, sizeof path);
    wsprintfA(cmd, "\"%s\" child", path);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
      child = pi.hProcess; CloseHandle(pi.hThread);
      L("CHILD: started (allocates and touches 80%% of RAM)\r\n");
    } else
      L("CHILD: CreateProcess FAILED, err %lu\r\n", (unsigned long)GetLastError());
  }
  DWORD prev[8] = {0}, stalls = 0;
  for (int t = 0; t < (sw ? 300 : hold ? 600 : 20); t++){
    if (hold && t % 50 == 0)
      L("HOLD : %d s, AP 0 rounds=%lu\r\n", t / 10, (unsigned long)jb[0]->rounds);
    if (child && WaitForSingleObject(child, 100) == WAIT_OBJECT_0){
      DWORD code = 0; GetExitCodeProcess(child, &code);
      L("CHILD: exited with %lu after ~%d ms\r\n", (unsigned long)code, t * 100);
      CloseHandle(child); child = NULL;
      t = 290;                   /* watch one more second */
    } else if (!child)
      Sleep(100);
    for (DWORD a = 0; a < n; a++){
      if (jb[a]->rounds == prev[a]) stalls++;
      prev[a] = jb[a]->rounds;
    }
  }
  if (child){ L("CHILD: still running, not waited for\r\n"); CloseHandle(child); }
  for (DWORD a = 0; a < n; a++){
    DWORD js[5]; vxd_job(h, a, js);
    L("WATCH: AP %lu rounds=%lu errors=%lu state=%s\r\n", (unsigned long)a,
      (unsigned long)jb[a]->rounds, (unsigned long)jb[a]->errors, job_state(js[0]));
  }
  L("WATCH: %lu samples without progress\r\n", (unsigned long)stalls);

  /* stop the jobs, wait for DONE, check every buffer against its last seed */
  for (DWORD a = 0; a < n; a++) jb[a]->stop = 1;
  int bad_total = 0;
  double want = x87_series();
  for (DWORD a = 0; a < n; a++){
    DWORD js[5];
    for (int t = 0; t < 100; t++){ vxd_job(h, a, js); if (js[0] >= 3) break; Sleep(10); }
    DWORD seed = jb[a]->last_seed, wrong = 0;
    for (DWORD i = 0; i < JOB_WORDS; i++)
      if (jb[a]->buf[i] != (seed ^ (i * 2654435761u))) wrong++;
    int x87ok = !memcmp((const void *)&jb[a]->x87, &want, sizeof want);
    L("DONE : AP %lu state=%s ret=%lu rounds=%lu, buffer wrong=%lu, AP errors=%lu, x87 %s\r\n",
      (unsigned long)a, job_state(js[0]), (unsigned long)js[1],
      (unsigned long)jb[a]->rounds, (unsigned long)wrong,
      (unsigned long)jb[a]->errors, x87ok ? "bit-identical" : "DIFFERENT");
    if (js[0] != 3 || wrong || jb[a]->errors || !x87ok || js[1] != jb[a]->rounds)
      bad_total++;
  }

  /* fault path: AP 0 page-faults on purpose and must park, reported */
  {
    DWORD st = vxd_run(h, 0, (int (__cdecl *)())fault_fn, NULL), js[5];
    for (int t = 0; t < 100; t++){ vxd_job(h, 0, js); if (js[0] >= 3) break; Sleep(10); }
    DWORD f[6] = {0}; DeviceIoControl(h, 5, NULL,0, f,sizeof f, &ret, NULL);
    L("FAULT: run %s, state=%s vector=%lu eip=%08lX cr2=%08lX, faults=%lu\r\n",
      st ? "REFUSED" : "posted", job_state(js[0]), (unsigned long)js[2],
      (unsigned long)js[3], (unsigned long)js[4], (unsigned long)f[5]);
    DWORD again = vxd_run(h, 0, (int (__cdecl *)())work, jb[0]);
    L("FAULT: RUN on the faulted AP -> status %lu (4 expected)\r\n", (unsigned long)again);
    if (js[0] != 4 || js[2] != 14 || js[4] != 0x7FFF0000 || again != 4) bad_total++;
  }

  DWORD p[6] = {0};
  DeviceIoControl(h, 6, NULL,0, NULL,0, &ret, NULL);
  DeviceIoControl(h, 5, NULL,0, p,sizeof p, &ret, NULL);
  L("STOP : parked=%lu of running=%lu\r\n", (unsigned long)p[3], (unsigned long)p[1]);
  DeviceIoControl(h, 9, NULL,0, NULL,0, &ret, NULL);
  DeviceIoControl(h, 3, NULL,0, NULL,0, &ret, NULL);
  VirtualFree(mem, 0, MEM_RELEASE);
  L("RESULT: %s\r\n", bad_total ? "FAILED" : "all jobs correct, fault reported");
  return bad_total != 0;
}

static const char *smp_err(DWORD s){
  switch(s){ case 0: return "ok";
    case 1: return "block _PageAllocate failed";
    case 2: return "APIC map/_MapPhysToLinear failed";
    case 3: return "no page below 1 MB for the trampoline";
    case 4: return "page directory build failed";
    case 5: return "CPU has no MSRs";
    case 6: return "APIC in x2APIC mode (MMIO IPIs would be ignored)";
    case 7: return "not running on the BSP";
    case 8: return "APIC base above 4 GB";
    case 9: return "no AP answered the SIPI";
    default: return "?"; }
}

int main(int argc, char **argv){
  static const struct { const char *arg, *log; DWORD xlo, xhi; } modes[] = {
    { "nostop", "VXDNOST.TXT", 0,     0 },
    { "x2apic", "VXDX2A.TXT",  0x400, 0 },
    { "notbsp", "VXDNBSP.TXT", 0x100, 0 },
    { "hi4g",   "VXDHI4G.TXT", 0,     1 },
    { "race",   "VXDRACE.TXT", 0,     0 },
    { "run",    "VXDRUN.TXT",  0,     0 },
    { "switch", "VXDSW.TXT",   0,     0 },
    { "hold",   "VXDHOLD.TXT", 0,     0 },
    { "observe","VXDOBS.TXT",  0,     0 },
  };
  if (argc > 1 && !_stricmp(argv[1], "child"))
    return run_child();
  const char *mode = "";
  DWORD xr[2] = {0,0};         /* START input: test XOR for MSR 1Bh lo/hi */
  for (unsigned i = 0; argc > 1 && i < sizeof modes / sizeof modes[0]; i++)
    if (!_stricmp(argv[1], modes[i].arg)){
      mode = modes[i].arg; g_log = modes[i].log;
      xr[0] = modes[i].xlo; xr[1] = modes[i].xhi;
    }
  int nostop = !strcmp(mode, "nostop");
  int race = !strcmp(mode, "race");
  DeleteFileA(g_log);
  L("=== vxdload%s%s ===\r\n", *mode ? " " : "", mode);
  if (xr[0] | xr[1])
    L("test: MSR 1Bh as seen by START XOR %08lX:%08lX\r\n",
      (unsigned long)xr[1], (unsigned long)xr[0]);
  HANDLE h = CreateFileA("\\\\.\\glsmp.vxd", 0, 0, NULL, 0,
                         FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (h == INVALID_HANDLE_VALUE){
    L("CreateFile failed, err %lu -> VxD did not load\r\n",(unsigned long)GetLastError());
    return 1;
  }
  L("VxD loaded, handle %p\r\n", h);
  if (!strcmp(mode, "observe")){
    DWORD prev[7] = {0}, st[7], r = 0;
    for (int t = 0; t < 180; t++){
      memset(st, 0, sizeof st);
      DeviceIoControl(h, 5, NULL,0, st,sizeof st, &r, NULL);
      if (t == 0 || memcmp(st, prev, sizeof st))
        L("%3d.%d s: alive=%lu running=%lu hb=%lu parked=%lu faults=%lu locks_left=%lu\r\n",
          t / 2, t % 2 * 5, (unsigned long)st[0], (unsigned long)st[1],
          (unsigned long)st[2], (unsigned long)st[3], (unsigned long)st[5],
          (unsigned long)st[6]);
      memcpy(prev, st, sizeof st);
      Sleep(500);
    }
    CloseHandle(h);
    L("closed. done\r\n");
    return 0;
  }
  if (!strcmp(mode, "run") || !strcmp(mode, "switch") || !strcmp(mode, "hold")){
    int rc = run_jobs(h, !strcmp(mode, "switch"), !strcmp(mode, "hold"));
    CloseHandle(h);
    L("closed (VxD unloaded). done\r\n");
    return rc;
  }

  DWORD echo=0, ret=0;
  if (DeviceIoControl(h, 1, NULL,0, &echo,sizeof echo, &ret, NULL))
    L("ECHO : out=%08lX\r\n", (unsigned long)echo);
  else
    L("ECHO : failed, err %lu\r\n", (unsigned long)GetLastError());

  DWORD info[4] = {0,0,0,0};   /* handle, linear, phys, pages */
  ret = 0;
  if (DeviceIoControl(h, 2, NULL,0, info,sizeof info, &ret, NULL) && ret >= 16){
    L("ALLOC: handle=%08lX linear=%08lX phys=%08lX pages=%lu (%lu KB)\r\n",
      (unsigned long)info[0],(unsigned long)info[1],(unsigned long)info[2],
      (unsigned long)info[3],(unsigned long)(info[3]*4));
    if (info[0]==0) L("ALLOC: _PageAllocate FAILED (handle 0)\r\n");
  } else {
    L("ALLOC: DeviceIoControl failed, err %lu, bytes %lu\r\n",
      (unsigned long)GetLastError(),(unsigned long)ret);
  }

  /* --- delay_ms in the VxD against QueryPerformanceCounter --- */
  LARGE_INTEGER qf, q0, q1;
  QueryPerformanceFrequency(&qf);
#define MS_SINCE(q) ((double)((q1).QuadPart - (q).QuadPart) * 1000.0 / (double)qf.QuadPart)
  {
    static const DWORD ms[] = { 1, 10, 20, 50 };
    for (unsigned i = 0; i < sizeof ms / sizeof ms[0]; i++){
      DWORD in = ms[i];
      QueryPerformanceCounter(&q0);
      DeviceIoControl(h, 7, &in,sizeof in, NULL,0, &ret, NULL);
      QueryPerformanceCounter(&q1);
      L("DELAY: delay_ms(%lu) took %.2f ms\r\n", (unsigned long)in, MS_SINCE(q0));
    }
  }

  HANDLE rt = NULL;
  if (race){
    g_h = h;
    DWORD tid = 0;   /* Win9x: CreateThread fails if lpThreadId is NULL */
    rt = CreateThread(NULL, 0, race_thread, NULL, 0, &tid);
    if (rt)
      L("RACE : second thread started, calling STATUS in a loop\r\n");
    else
      L("RACE : CreateThread FAILED, err %lu\r\n", (unsigned long)GetLastError());
  }

  /* --- SMP: start the APs, watch the heartbeat, stop them --- */
  DWORD s[10] = {0};  /* alive, running, hb, tramp phys, apic lin, status,
                         usable, siblings, SMT shift, BSP core */
  ret = 0;
  QueryPerformanceCounter(&q0);
  BOOL ok = DeviceIoControl(h, 4, xr,sizeof xr, s,sizeof s, &ret, NULL);
  QueryPerformanceCounter(&q1);
  L("START: took %.2f ms (10 + 1 + 20 ms waits)\r\n", MS_SINCE(q0));
  if (ok && ret >= 24){
    L("START: status=%lu (%s)\r\n", (unsigned long)s[5], smp_err(s[5]));
    L("START: tramp phys=%08lX  APIC linear=%08lX\r\n",
      (unsigned long)s[3], (unsigned long)s[4]);
    L("START: alive=%lu (reached trampoline)  running=%lu (reached ap_pm32)  hb=%lu\r\n",
      (unsigned long)s[0], (unsigned long)s[1], (unsigned long)s[2]);
    if (ret >= 40)
      L("START: usable=%lu (one per physical core)  HT siblings parked=%lu  SMT shift=%lu  BSP core=%lu\r\n",
        (unsigned long)s[6], (unsigned long)s[7], (unsigned long)s[8], (unsigned long)s[9]);
  } else {
    L("START: DeviceIoControl failed, err %lu, bytes %lu\r\n",
      (unsigned long)GetLastError(),(unsigned long)ret);
  }

  /* heartbeat must advance between two reads if the APs really run our code */
  {
    DWORD a[5]={0,0,0,0,0}, b[5]={0,0,0,0,0};
    DeviceIoControl(h, 5, NULL,0, a,sizeof a, &ret, NULL);
    Sleep(200);
    DeviceIoControl(h, 5, NULL,0, b,sizeof b, &ret, NULL);
    L("STATUS: alive=%lu running=%lu  heartbeat %lu -> %lu (%s)\r\n",
      (unsigned long)b[0], (unsigned long)b[1],
      (unsigned long)a[2], (unsigned long)b[2],
      b[2] > a[2] ? "ADVANCING - APs are executing VxD code" : "stuck");
    /* the APIC is switched on only inside each IPI send (ipi_once) */
    L("STATUS: MSR 1Bh=%08lX, APIC global enable %s while the APs run\r\n",
      (unsigned long)b[4], (b[4] & 0x800) ? "ON (unexpected)" : "off as Windows left it");
  }

  if (nostop)
    L("STOP : skipped (FREE must stop the APs itself)\r\n");
  else {
    QueryPerformanceCounter(&q0);
    ok = DeviceIoControl(h, 6, NULL,0, NULL,0, &ret, NULL);
    QueryPerformanceCounter(&q1);
    if (ok) {
      DWORD p[12]={0};
      DeviceIoControl(h, 5, NULL,0, p,sizeof p, &ret, NULL);
      L("STOP : ok in %.2f ms, parked=%lu of running=%lu (%s)\r\n", MS_SINCE(q0),
        (unsigned long)p[3], (unsigned long)p[1],
        p[3] == p[1] ? "all halted before INIT" : "INIT alone");
      L("STOP : ROM hlt loop at %05lX, %lu APs parked there\r\n",
        (unsigned long)p[11], (unsigned long)p[10]);
    } else
      L("STOP : failed, err %lu\r\n", (unsigned long)GetLastError());
  }

  if (DeviceIoControl(h, 3, NULL,0, NULL,0, &ret, NULL))
    L("FREE : ok\r\n");
  else
    L("FREE : failed, err %lu\r\n", (unsigned long)GetLastError());

  /* after FREE the APs must be stopped: heartbeat frozen */
  {
    DWORD a[4]={0,0,0,0}, b[4]={0,0,0,0};
    DeviceIoControl(h, 5, NULL,0, a,sizeof a, &ret, NULL);
    Sleep(200);
    DeviceIoControl(h, 5, NULL,0, b,sizeof b, &ret, NULL);
    L("AFTER FREE: heartbeat %lu -> %lu (%s), parked=%lu of running=%lu\r\n",
      (unsigned long)a[2], (unsigned long)b[2],
      b[2] == a[2] ? "frozen - APs stopped" : "STILL RUNNING",
      (unsigned long)b[3], (unsigned long)b[1]);
  }

  if (rt){
    g_race_stop = 1;
    WaitForSingleObject(rt, INFINITE);
    CloseHandle(rt);
    L("RACE : second thread STATUS calls: ok=%ld busy=%ld other=%ld (last other err %lu)\r\n",
      (long)g_race_ok, (long)g_race_busy, (long)g_race_other, (unsigned long)g_race_other_err);
  }

  CloseHandle(h);
  L("closed (VxD unloaded). done\r\n");
  return 0;
}
