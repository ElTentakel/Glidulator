/*
 * Starts a process with a DLL loaded into it, on Windows 9x as well
 * (no VirtualAllocEx/CreateRemoteThread there).
 *
 * 1. Create the process suspended and put "jmp $" (EB FE) on the EXE's
 *    entry point; resume until the main thread spins there.  At that point
 *    the loader has resolved the imports and initialised the DLLs.
 * 2. Write a stub that calls LoadLibraryA (kernel32 has the same address
 *    in every process) over the entry code, run it, restore the code and
 *    continue at the entry point.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "inject.h"

static DWORD exe_entry(const char *exe, DWORD *base)
{
    HANDLE f = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    unsigned char h[1024];
    DWORD n = 0;
    ReadFile(f, h, sizeof h, &n, NULL);
    CloseHandle(f);
    if (n < 64 || h[0] != 'M' || h[1] != 'Z')
        return 0;
    DWORD pe = *(DWORD *)(h + 0x3c);
    if (pe + 0x58 > n || memcmp(h + pe, "PE\0\0", 4))
        return 0;
    *base = *(DWORD *)(h + pe + 0x34);
    return *base + *(DWORD *)(h + pe + 0x28);
}

/* Extracts the EXE path from CreateProcess arguments. */
static void app_path(const char *app, const char *cmd, char *out, int size)
{
    if (app) {
        lstrcpynA(out, app, size);
        return;
    }
    const char *p = cmd;
    int i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < size - 1)
            out[i++] = *p++;
    } else {
        while (*p && *p != ' ' && i < size - 1)
            out[i++] = *p++;
    }
    out[i] = 0;
    /* add .exe when missing; SearchPath finds relative names */
    char full[MAX_PATH], *fp;
    if (SearchPathA(NULL, out, ".exe", sizeof full, full, &fp))
        lstrcpynA(out, full, size);
}

BOOL inject_create(const char *app, char *cmd, SECURITY_ATTRIBUTES *pa,
                   SECURITY_ATTRIBUTES *ta, BOOL inherit, DWORD flags,
                   void *env, const char *dir, STARTUPINFOA *si,
                   PROCESS_INFORMATION *pi, const char *dll,
                   create_fn create, log_fn log)
{
    if (!create(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env,
                dir, si, pi))
        return FALSE;

    char exe[MAX_PATH];
    app_path(app, cmd, exe, sizeof exe);
    DWORD base, entry = exe_entry(exe, &base);
    unsigned char orig[2], loop[2] = { 0xEB, 0xFE };
    DWORD n;
    if (!entry) {
        log("inject: no PE entry point for %s", exe);
        goto done;
    }
    if (!ReadProcessMemory(pi->hProcess, (void *)entry, orig, 2, &n) ||
        !WriteProcessMemory(pi->hProcess, (void *)entry, loop, 2, &n)) {
        log("inject: cannot patch entry %08lx of %s (error %lu)", entry, exe,
            GetLastError());
        goto done;
    }
    FlushInstructionCache(pi->hProcess, (void *)entry, 2);

    CONTEXT c;
    int ok = 0;
    for (int i = 0; i < 1000 && !ok; i++) {
        ResumeThread(pi->hThread);
        Sleep(10);
        SuspendThread(pi->hThread);
        c.ContextFlags = CONTEXT_FULL;
        GetThreadContext(pi->hThread, &c);
        ok = c.Eip == entry;
    }
    if (!ok) {
        WriteProcessMemory(pi->hProcess, (void *)entry, orig, 2, &n);
        log("inject: %s did not reach its entry point", exe);
        goto done;
    }

    /* The stub replaces the entry code for a moment (code pages are
     * executable everywhere, the stack is not under NX):
     *   pushfd; pushad; push path; mov eax, LoadLibraryA; call eax;
     *   popad; popfd; jmp $
     * Once the thread spins on the final jmp, the original bytes go back
     * and EIP returns to the entry point with all registers unchanged. */
    DWORD lla = (DWORD)GetProcAddress(GetModuleHandleA("KERNEL32.DLL"),
                                      "LoadLibraryA");
    DWORD path_at = entry + 32;
    unsigned char stub[32 + MAX_PATH], save[32 + MAX_PATH];
    unsigned char *s = stub;
    *s++ = 0x9C;
    *s++ = 0x60;
    *s++ = 0x68; memcpy(s, &path_at, 4); s += 4;
    *s++ = 0xB8; memcpy(s, &lla, 4); s += 4;
    *s++ = 0xFF; *s++ = 0xD0;
    *s++ = 0x61;
    *s++ = 0x9D;
    DWORD spin = entry + (DWORD)(s - stub);
    *s++ = 0xEB; *s++ = 0xFE;
    memset(s, 0xCC, stub + 32 - s);
    int len = 32 + lstrlenA(dll) + 1;
    memcpy(stub + 32, dll, len - 32);
    if (!ReadProcessMemory(pi->hProcess, (void *)entry, save, len, &n) ||
        !WriteProcessMemory(pi->hProcess, (void *)entry, stub, len, &n)) {
        WriteProcessMemory(pi->hProcess, (void *)entry, orig, 2, &n);
        log("inject: cannot write the stub (error %lu)", GetLastError());
        goto done;
    }
    /* the first two saved bytes are our EB FE, the originals are in orig */
    memcpy(save, orig, 2);
    FlushInstructionCache(pi->hProcess, (void *)entry, len);
    c.Eip = entry;
    SetThreadContext(pi->hThread, &c);
    ok = 0;
    for (int i = 0; i < 1000 && !ok; i++) {
        ResumeThread(pi->hThread);
        Sleep(10);
        SuspendThread(pi->hThread);
        c.ContextFlags = CONTEXT_FULL;
        GetThreadContext(pi->hThread, &c);
        ok = c.Eip == spin;
    }
    WriteProcessMemory(pi->hProcess, (void *)entry, save, len, &n);
    FlushInstructionCache(pi->hProcess, (void *)entry, len);
    if (!ok) {
        log("inject: LoadLibrary stub did not finish in %s", exe);
        goto done;
    }
    c.Eip = entry;
    SetThreadContext(pi->hThread, &c);
    log("inject: %s (pid %08lx), entry %08lx, loaded %s", exe,
        pi->dwProcessId, entry, dll);
done:
    if (!(flags & CREATE_SUSPENDED))
        ResumeThread(pi->hThread);
    return TRUE;
}
