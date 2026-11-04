# Glidulator GL-1 Changes

*ElTentakel Graphics Systems, Product Change Notices*

Newest release first. Known issues of the current release:
[docs/compatibility.md](docs/compatibility.md).

## 0.9.4 (2026-11-04, preview)

- **`GLIDULATOR_HT=1`** (experimental, DOS and Windows 9x): Hyper-Threading
  siblings run render workers too, real cores first. Since 0.9.3 the
  siblings stay idle, which halved the frame rate on 2 cores + HT with the
  BIOS option "CPUID Limit" (0.9.2 counted them as cores there by mistake;
  tester report, VOGONS).
- **One render worker on two cores**: with two cores (one physical core
  per worker, see above) everything was rendered on the game's core; now
  one worker draws while the game prepares the next triangles (OSD `2/2
  CORES`). Nature on an i7-4790 limited to 2 cores: 20.8 -> 21.9 fps;
  games with more work of their own gain more. `GLIDULATOR_THREADS=1`
  keeps the old behaviour.
- **Multi-core after Windows 98's "Restart in MS-DOS mode" under
  HDPMI32i** (also with EMM386): Windows leaves the local APIC switched
  off, and only ring 0 can switch it on. The OVL now reaches ring 0 for
  that through a short-lived call gate in the host's GDT (before: one core,
  tester report, VOGONS).
- **Write-combining on machines with much RAM** (DOS and Windows 98):
  firmware spends variable MTRRs on RAM and holes above 4 GB, so the
  framebuffer did not fit (`too few free MTRRs`, e.g. i7-4790 with 32 GB).
  Without PAE nothing above 4 GB is reachable, so those ranges are left
  out while the game runs. i7-4790: GLQuake under Windows 98 (VBEMP)
  37.6 -> 56.3 fps; Nature under DOS (800x600) output 17.1 -> 2.8 ms per
  frame, 26.4 -> 48.7 fps at the same scene.
- **Fixed: Half-Life (and other games) could freeze after a minute or
  two with several cores** (picture stands, the game "not responding").
  The main core could overwrite a queued triangle while a render worker
  was still drawing it; the texture-cache counters then never matched and
  the game waited forever for a free texture slot. Affects DOS and
  Windows 9x with 3 or more cores; write-combining made it more likely
  (faster output, fuller queue).
- **Windows 98: original MTRRs are restored on exit again.** The check
  that nothing else changed them compared the wrong bytes and always
  failed, so the write-combining layout stayed until the next reboot.

## 0.9.3 (2026-10-28, preview)

- Credits for PCem/86Box, MGA-Glide and the Glide source release moved
  to the top of the README and into the package README.TXT.
- **`GLIDULATOR_SCREENSHOT=2`**: screenshots include the OSD (user wish).
- **OSD shows how the other cores run**: `SMP RING0`, `PAGING`, `FLAT`
  (DOS), `GLSMP` (Windows 9x), `THREADS` (Windows NT/XP, Wine) or `OFF`
  (user wish).
- **Hyper-Threading with the BIOS option "CPUID Limit"** (DOS and Windows
  9x): the siblings were counted as cores of their own, so workers shared
  a core with each other or with the game (4 cores + HT: 5/8 cores, slower
  than 4/4). Intel CPUs from Nehalem on are now recognised correctly with
  the limit on (tester report, VOGONS).
- **8.4 MB less memory**: the texture cache no longer reserves an unused
  256x256 area per slot (about 19 MB instead of 28 MB in total). Pył
  failed with `CH_C3DFX[80]` when loading a level without HDPMI32i; it now
  loads (tester report, VOGONS).
- **Mipmap dithering** (`GLIDULATOR_LOD_DITHER=auto|1|0`): the board's
  LOD dither was not emulated, so games asking for
  `GR_MIPMAP_NEAREST_DITHER` showed hard bands between mipmap levels. Now
  the level edge becomes a 4x4 dither pattern as on the Voodoo; `1` forces
  it for every game, like the option of 3dfx's drivers (user wish).
- The rasterizer's known issue "single pixels open on shared triangle
  edges" is withdrawn: shared edges are drawn exactly once. The gaps came
  from the start-up logo, whose fans computed the same corner twice with
  slightly different results. The logo is fixed; a new test checks fans.

## 0.9.2 (2026-10-18, preview)

- **Multi-core rendering under Windows 95/98/ME**: copy `GLSMP.VXD` next
  to `glide2x.dll`. The DLL loads it only while the game runs and starts
  the other CPU cores through it, as the DOS module does; nothing is
  installed. GLQuake's `timedemo demo1` on a Core i7-4770T: 38.8 fps
  instead of 15.4 fps. `GLIDULATOR_SMP=off` turns it off.
- Windows 9x: write-combining for the display adapter's framebuffer
  with generic display drivers such as VBEMP (several times faster output;
  `GLIDULATOR_WC=0` turns it off). Tested in a virtual machine so far.
- The trace DLL no longer crashes on start with programs whose window
  class name is long (Nature95).

## 0.9.1 (2026-10-17, preview)

- Windows 98: GLQuake no longer freezes when its first demo starts.
- `GLIDULATOR_BOARD=rush` reports a Voodoo Rush, for Rush patches such as
  Tomb Raider's `TUMBRUSH.EXE`.
- DOS: Print Screen saves the current frame as a BMP
  (`GLIDULATOR_SCREENSHOT`).
- DOS: multi-core rendering after Windows 98's "Restart in MS-DOS mode"
  (Windows leaves the local APIC switched off; Glidulator switches it back
  on for the game's run). Tested in a virtual machine so far.
- DOS: the other cores are parked in a BIOS halt loop after the game, which
  fixes a hang ("internal stack overflow") on some machines after a
  multi-core run.
- The on-screen display shows the cores in use and available (`1/2 CORES`)
  instead of the number of render threads.

## 0.9.0 (2026-10-11, first public preview)

- `GLIDE2X.OVL` for DOS games (DOS/4G LE DLL) and `glide2x.dll` for
  Windows 9x games: all 132 Glide 2.x entry points and the 14 `pci*`
  helpers of the original runtime.
- Register-accurate rasterizer (ported from PCem/86Box), gamma like the
  Voodoo RAMDAC, 4x4 dithering like the chip.
- Multi-core rendering with up to four render workers, also under plain
  DOS, where the OVL starts the additional CPU cores itself.
- x86 JIT: the pixel pipeline compiled to SSE2 machine code per render
  state.
- Image options: FSAA (rotated grid, 2x/4x/auto), edge smoothing, the
  Voodoo Graphics video output filter.
- Start-up show: the Glidulator animation or 3dfx's own logo from an
  original Glide file, optionally with an analog monitor effect.
- Aspect-correct scaling, any display mode, on-screen display.
- Trace and debug variants: performance logging, frame capture and
  replay, crash handling on real hardware.
