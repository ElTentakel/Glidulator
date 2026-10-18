# Glidulator GL-1 Development Guide

*ElTentakel Graphics Systems, Engineering Department (one person, eight
arms)*

## Toolchains

| Target | Compiler | Notes |
|---|---|---|
| `core/` + tests | GCC (Linux) | ASan/UBSan, `libpng-dev` for the test harness only |
| `glide2x.dll` | mingw-w64 `i686-w64-mingw32-gcc` | `-D_WIN32_WINNT=0x0400`, libgcc static |
| `GLIDE2X.OVL` | Open Watcom v2 in `/opt/watcom` | Pinned snapshot `2026-09-01-Build`. Warnings are errors (`-we`). |
| `glide_dos_test.exe` | DJGPP in `/opt/djgpp` | Build check of the core with a third compiler. |
| AP trampoline | `gcc -m32` + `objcopy` + `xxd` | Generates `build/ovl/ap_tramp.h`. |
| `GLSMP.VXD` | Open Watcom `wasm` + `wlink` | `form windows vxd dynamic`, exactly one export (the DDB). Hand-written assembly: `wasm` does not accept the DDK's `VMM.INC`, so the DDB layout and VMM service numbers are written out. Trampoline: `gcc -m32` + `objcopy` + `od`, included as `db` lines (`build/win98vxd/aptramp.inc`). |

Watcom specifics the code relies on:
- Calling-convention and export macros go after the return type
  (`void GR_EXPORT GLIDE_API name(...)`).
- The OVL has no CRT startup, so do not call `malloc`, `printf` or
  `fopen`. Use `platform_*`.
- A dummy `main` satisfies `clib3s.lib`.
- No C99 float functions (`lrintf` etc.). Round with `glide_round32/64`;
  Watcom's `llrint` returns 0 beyond 32 bits.
- Inline assembly uses `#pragma aux`.

## Build targets

```
make test             # Linux build, Glide test harness (tests 1-22), video and MTRR unit tests
make ovl              # build/ovl/GLIDE2X.OVL          release
make ovl-trace        # build/ovl_trace/GLIDE2X.OVL    -dGLIDULATOR_TRACE
make ovl-debug        # build/ovl_debug/GLIDE2X.OVL    -dGLIDULATOR_TRACE -dGLIDULATOR_DEBUG
make ovl-all          # all three
make win98            # build/win98/glide2x.dll (also win98-trace, win98-debug) and the VxD
make vxd              # build/win98vxd/GLSMP.VXD only
make win98-all        # all three
make dist             # release package, see below
make dos              # DJGPP build check
make replay           # build/linux/glide_replay (64-bit, C path)
make replay32         # build/linux32/glide_replay (32-bit, with JIT)
make bench            # replay timing and path comparison (-O2, no sanitizers)
make ovl-demo         # load the real OVL in a 32-bit Linux process (leload) and render
make dosbox-test      # load the OVL in DOSBox (VBE and mode 13h, DOS/4GW and DOS/32A)
```

All targets must build without warnings. The `OVL_DEFS`/`WIN98_DEFS`
variants have their own build directories. Make does not track define
changes, so delete the directory when switching defines by hand.

## Release package

The version number lives in `core/include/glidulator/version.h` only.
`make dist` runs `make test`, builds all six binaries and writes:

- `build/dist/glidulator-<version>.zip`: `BIN\`, `BIN\TRACE\`,
  `BIN\DEBUG\` (OVL, DLL and `GLSMP.VXD` each, DLLs stripped), the example
  `GLIDULAT.CFG` and the texts from `package/` and `docs/` with CRLF line
  ends and 8.3 names, so the folder can be copied to a DOS machine as is.
- `build/dist/glidulator-<version>-symbols.zip`: the DLLs with symbols and
  the OVL map files. Keep it for every package handed out; crash addresses
  in a user's `GLIDULAT.LOG` are resolved with it
  (`tools/dosvm/ovladdr.py`).

The DLL carries the version in its VERSIONINFO resource
(`targets/glide_dll/glide2x.rc`); the trace and debug variants write it as
the first line of each session in `GLIDULAT.LOG`. Test 16 checks that
`package/GLIDULAT.CFG` lists every setting, commented out, within the
reader's 8 KB limit; add new settings there and to the User Guide.

## Tests

**`make test`** (`targets/glide_test_linux`, ASan/UBSan) renders with the
real core and checks pixels. Reference images go to `tests/golden/`;
`git status` must show them unchanged. The tests cover:
- triangles and textures (all formats)
- Z/W buffer, mipmaps, perspective
- combine modes and blending
- LFB access in every format
- board queries and state save/restore
- `guTex*` and `.3DF` files
- fog and NCC
- clipping
- renderer selection
- output rectangle and OSD
- FSAA, `auto`, edge smoothing and the video filter, with 1 and n workers
- writes through an LFB pointer after `grSstWinClose` (ASan)
- start-up show: frames presented, initial state restored afterwards,
  monitor effect with and without logo (and the older `SPLASH=crt`);
  frames in `build/linux/splash_*.png`. The original 3dfx animation runs
  with `GLIDULATOR_TEST_SPLASH_FILE=<original GLIDE2X.OVL/dll>`.

**`make ovl-demo`** loads the Watcom-built OVL through a clean-room LE
loader (`tools/leload`) in a Linux process with a simulated VBE/VGA BIOS
(including a program that returns to text mode before shutting Glide
down).
It exercises the real export table and the complete output chain.

**`make dosbox-test`** runs `OVLTEST.EXE` under real DOS/4GW and DOS/32A
in DOSBox. It verifies the picture by screenshot, plus texture memory,
`.3DF` loading and vsync timing.

## Working with real software

| Tool | Purpose |
|---|---|
| `tools/compat/run_nature.sh`, `run_demo.sh <name>` | DOS demos in headless DOSBox with the debug OVL; screenshots, `GLIDULAT.LOG`, frame captures. Demos are downloaded to `build/compat/` and never committed. |
| `tools/compat/play_nature.sh`, `play_demo.sh` | The same, in a visible window. |
| `tools/compat/run_windemo.sh <dir> <exe> [n] [gap] [keys]` | Windows programs under 32-bit Wine on Xvfb. Virtual desktop, Windows 98 version, key/mouse scripting, screenshots. `TRACE=1`/`DEBUG=1` select the DLL variant; `WINEDEBUG` is passed through (e.g. `+relay`). |
| `tools/compat/get_glquake.sh` | Fetches Quake 1.06 shareware and GLQuake 0.97 with the 3dfx MiniGL. Installers are unpacked with `tools/compat/unpack/` (SZDD, LHA `-lh5-`), never executed. |
| `tools/dosbox_test/session.py` | Drives DOSBox with scripted keys (XTest) and screenshots. |
| `tools/dosvm/run.sh` | Nature in QEMU on FreeDOS with selectable CPU count/topology, XMS driver and OVL. The only way to test multi-core DOS rendering without hardware (TCG timing is not meaningful). Driver `sbemu` runs it under SBEMU with JEMMEX/HDPMI32i and an AC97 card (`SBEMUARGS`, `WAV=1` records the card output); `vdpmi` needs VME and fails under TCG (use `QEMU_OPTS="-enable-kvm -cpu host -smp 4"`). `R3PROBE=AP NATUREONLY=0` runs `r3probe.c` instead: what the DPMI host lets a ring-3 client see (CR0/CR3/CR4, page walk, local APIC, SSE) and a guarded start of the other cores on the host's address space. |
| `glide_replay <dir> CAPnn.BIN out.png` | Re-render a captured frame; `--list`, `--only N`, `--upto N`, `--compare`, `--difftri`, `--bench N`. Honours all `GLIDULATOR_*` settings. |
| `tools/dosvm/ovladdr.py <map> <log>` | Map code addresses in a debug log to OVL functions (keep the map of every OVL handed out). |
| `tools/win98vxd/build.sh` | Windows 9x test programs for `GLSMP.VXD` (`VXDLOAD.EXE`: the VxD alone; `GLMTTEST.EXE`: multi-core against one core through the DLL, pixel by pixel). See its README. |

**Rules of thumb**:
- When behaviour is unclear, read 3dfx's Glide 2.x source (sezero/glide)
  instead of guessing.
- Separate "the game sends wrong data" from "our pipeline is wrong" from
  "only the Watcom build is wrong". To do that, capture with the debug
  variant and replay on Linux with both paths.

## Hardware test setups

Real hardware is tested with a bootable USB stick (Windows 98
QuickInstall, FreeDOS with boot variants for memory managers and SBEMU,
all variants and the tested demos, a demo menu) and a local Windows 98 VM
under KVM driven by scripted keys and screenshots. Both contain
third-party software and tester texts in German and are kept in the
maintainer's internal repository only.

## Repository layout

```
abi/            clean-room Glide 2.x API description (MIT, MGA-Glide)
core/           portable implementation; core/rasterizer/ = PCem/86Box port
platform/       dos_ovl, win98, linux, dos
targets/        glide_ovl, glide_dll, glide_test_linux, glide_replay, test programs
tests/          unit tests, golden images
tools/          ABI generator, LE tools, test runners
package/        texts of the release package (README.TXT, example GLIDULAT.CFG)
docs/           this documentation
```

Source comments and log output are in English. Comments record design
decisions and problem/solution notes, not the history of how they came
about. Development happens in an internal repository with the full
history and working notes; the public repository
(<https://github.com/ElTentakel/Glidulator>) receives one commit per
release.
