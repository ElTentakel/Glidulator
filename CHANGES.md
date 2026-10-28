# Glidulator GL-1 Changes

*ElTentakel Graphics Systems, Product Change Notices*

Newest release first. Known issues of the current release:
[docs/compatibility.md](docs/compatibility.md).

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
