# Windows 9x multi-core: test programs for GLSMP.VXD

`GLSMP.VXD` (source `platform/win98/vxd/`, built by `make vxd` and with every
`make win98*`) runs the render workers of `glide2x.dll` on the other CPU
cores under Windows 95/98/ME. Design: `docs/technical-reference.md`,
"Multi-core under Windows 9x". This directory holds two Win32 test programs
for it, built by `tools/win98vxd/build.sh` into `build/win98vxd/`:

| Program | What it does | Log |
|---|---|---|
| `VXDLOAD.EXE` | Loads the VxD and drives it directly: starts the cores, checks that they run, stops them and frees everything | `VXDLOAD.TXT` |
| `VXDLOAD run` | Runs a function of `VXDLOAD.EXE` on every usable core over locked private memory; checks the buffers and an x87 result bit for bit, then a deliberate page fault must come back as vector 14 | `VXDRUN.TXT` |
| `VXDLOAD switch` | As `run`, while a child process allocates and touches 80% of the RAM (process switches, paging) | `VXDSW.TXT` |
| `VXDLOAD hold` / `observe` | `hold` runs the jobs for 60 s so the process can be killed (Ctrl+Alt+Del); `observe`, started first, logs the VxD state meanwhile: the cores must stop and the locks must be released | `VXDHOLD.TXT`, `VXDOBS.TXT` |
| `VXDLOAD x2apic`, `notbsp`, `hi4g`, `nostop`, `race` | Test hooks for the refusal paths (x2APIC, not the boot core, APIC above 4 GB), FREE without STOP, and a second thread calling STATUS meanwhile | `VXDX2A.TXT`, ... |
| `GLMTTEST.EXE` | Renders a deterministic scene through `glide2x.dll` (texture, Gouraud, blending, depth buffer) with the JIT and the C path, with all cores and with one, and compares every frame pixel by pixel with the one-core C reference | `GLMTTEST.TXT` |

Copy the programs together with `GLSMP.VXD` (and, for `GLMTTEST`, a
`glide2x.dll`, preferably the trace variant, which logs the `Multi-core:`
lines to `GLIDULAT.LOG`) into one directory on a Windows 9x machine and run
them from the Start menu's Run box. They are GUI programs on purpose: some
display drivers garble the desktop when a console window opens.

## DeviceIoControl protocol

| Code | Input | Output | Effect |
|---|---|---|---|
| 1 ECHO | | `CAFEBABE` | smoke test |
| 2 ALLOC | | `{handle, linear, physical, pages}` | the VxD's small fixed block (AP page directory, identity page table, IDT) and the AP stacks |
| 3 FREE | | | stops the cores first, then frees |
| 4 START | `{xor lo, xor hi}` (test hook, normally 0) | `{alive, running, heartbeat, trampoline phys, APIC linear, status, usable, HT siblings, SMT shift, BSP core}` | INIT/SIPI to all other cores |
| 5 STATUS | | `{alive, running, heartbeat, parked, MSR 1Bh, faults, locks left, PDEs added, PDE clashes, locks, ROM-parked, ROM hlt address, PDE resyncs}` | |
| 6 STOP | | | cores leave their loop, INIT, park in the BIOS ROM's hlt loop |
| 7 DELAY | `{ms}` | | timing check of the VxD's wait |
| 8 LOCK | `{linear, bytes}` | `{status}` | `_LinPageLock` in the calling process |
| 9 UNLOCK | `{linear, bytes}` or nothing | `{status}` | that range, or all |
| 10 RUN | `{core, fn, arg, x87 control word, MXCSR}` | `{status}` | core calls `fn(arg)` (cdecl) |
| 11 JOB | `{core}` | `{state, return value, vector, EIP, CR2}` | state 0 idle, 1 posted, 2 running, 3 done, 4 CPU exception |
| -1 CLOSEHANDLE | | | sent by VWIN32 when the handle closes, also when Windows ends a killed process: stops the cores, unlocks, frees |

START status: 0 ok, 1-4 an allocation or mapping failed, 5 no MSRs,
6 x2APIC mode, 7 not on the boot core, 8 APIC above 4 GB, 9 no core answered.
