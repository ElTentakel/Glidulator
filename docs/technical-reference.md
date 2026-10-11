# Glidulator GL-1 Technical Reference

*ElTentakel Graphics Systems, Publication No. ETGS-GL1-TR*

This document describes how the GL-1 works and why it is built the way it
is. It records final design decisions only.

## 1. Overview

```
Game
  |  Glide 2.x calls (grSstWinOpen, grDrawTriangle, ...)
  v
targets/glide_ovl (DOS, GLIDE2X.OVL)      targets/glide_dll (Windows, glide2x.dll)
  |  exports, entry point; no own state
  v
core/                                      portable, no OS knowledge
  glide_*.c       Glide API -> Voodoo registers ("register driver")
  render_mt.c     triangle queue for render workers
  rasterizer/     Voodoo Graphics rasterizer and texture unit (86Box), x86 JIT
  fsaa.c          sub-pixel planes, resolve, edge smoothing
  vfilter.c       Voodoo video output filter
  osd.c           output timing, on-screen display
  |  platform_* calls (core/include/glidulator/platform.h)
  v
platform/dos_ovl   Open Watcom: DPMI, VBE/VGA, MTRR write-combining, CPU cores
platform/win98     mingw-w64: GDI output, threads
platform/linux     reference platform for tests
platform/dos       DJGPP build check
```

`core/` contains no `#ifdef` for operating systems. Everything
system-specific goes through `platform.h`:
- memory and time
- configuration and logging
- core count and workers
- display open/close
- `platform_blit` (RGB565) and `platform_blit32` (XRGB8888)
- gamma

## 2. Glide as a register driver

3dfx's Glide translated API calls into writes to the Voodoo's registers
(`fbzMode`, `fbzColorPath`, `alphaMode`, `fogMode`, `textureMode`, `tLOD`,
`texBaseAddr`, `color0/1`, ...). The GL-1 does exactly the same:

- **State calls** (`grColorCombine`, `grDepthBufferMode`, `grTexSource`,
  ...) set bits in a register image (`g_voodoo.params`).
- **`grDrawTriangle`** computes the triangle setup registers as the chip's
  setup unit would: start values and gradients of colour, Z, W and S/T/W
  as plane equations in the chip's fixed-point formats.
- **The rasterizer** evaluates these registers exactly like the hardware.
  It was ported from 86Box, which emulates the chip register-accurately.

This makes behaviour questions answerable by reference: what does 3dfx's
Glide 2.x source write, and what does the chip do with it. The 3dfx source
release is the reference for every API detail.

Notable consequences:
- Vertex coordinates are 12.4 fixed point and wrap like on the chip. This
  is why games add a "snap bias" to coordinates, and why the `gu*WithClip`
  functions really clip.
- `grSstQueryHardware` reports a Voodoo Graphics with 2 MB texture memory
  per TMU, one TMU. Games quit if no board is found.
- Gamma defaults to 1.3, the value of later 3dfx drivers. Configurable via
  `SST_GAMMA`.

ABI: all 132 Glide 2.x functions and the 14 `pci*` functions of 3dfx's
runtime are exported under their original decorated names. 103 Glide
functions are implemented. The rest are rarely used and exported as
harmless stubs:
- antialiased primitives `grAA*`
- `guMP*`, `guMovie*`
- `grSplash`
- RLE16 and multi-base textures

Declarations are generated from a clean-room API description
(`abi/glide2x.api`, `tools/abi/gen_glide2x.py`).

## 3. Module formats

- **`GLIDE2X.OVL` is not an overlay but a DOS/4G LE DLL**. Games load it
  through the extender (`LINEXE_LOADMODULE`) and resolve functions by name
  (`_GRDRAWTRIANGLE@12`). Only Open Watcom's `wlink` produces this format,
  so the OVL is built with Open Watcom. `tools/abi/lefix.py` sets module
  name and flags the way games expect.
- The OVL has no C runtime startup. Memory comes from DPMI (`INT 31h`
  0501h) per allocation, and nothing in the OVL relies on CRT
  initialisation.
- **`glide2x.dll`** is built with mingw-w64 for Windows 4.0. Exports carry
  3dfx's exact names and mixed casing (`targets/glide_dll/glide2x.def`),
  libgcc is linked statically, and only `KERNEL32` and `msvcrt` are needed.

## 4. Rendering paths

Two independent axes, chosen at `grSstWinOpen` (`core/renderer.c`):

| Axis | Choices |
|---|---|
| Pixel pipeline | **C** interpreter (always available), **JIT**: x86 code generated per render-state combination with SSE2 (32-bit builds only) |
| Threads | 1, or 2-4 render workers |

Both paths and all thread counts produce bit-identical frames. This is
verified on recorded frames of real software (`glide_replay --compare`).

**SSE availability**:
- Windows: the DLL runs one SSE instruction behind its own SEH frame at
  start-up. Windows 98SE and ME save the SSE registers (JIT); Windows 95
  and the first Windows 98 do not, and the DLL falls back to the C path
  instead of crashing on the first SSE instruction of the JIT.
- Under DOS, the OVL enables `CR4.OSFXSR` itself when it runs in ring 0 and
  restores it on close. In ring 3 it runs one SSE instruction behind a DPMI
  exception handler: HDPMI32i and the DOS box of Windows 98SE have the bit
  set (JIT), VDPMI does not (C path).
- AVX is never used: neither Windows 9x nor DOS saves the upper YMM halves.

## 5. Multi-core rendering

`core/render_mt.c` keeps a queue of up to 256 prepared triangles. Every
worker draws every triangle, but only its own scanlines (`y % n == i`, as
86Box does). The game continues while workers draw. It waits only at
synchronisation points:
- buffer swap
- buffer clear
- direct framebuffer access
- texture-cache pressure
- close

**Workers under DOS** (`platform/dos_ovl/smp_dos.c`): DOS has no
scheduler, so the OVL starts the application processors (APs) the way an
operating system does:
1. **Start**: INIT IPI, 10 ms, two Startup IPIs to a 4 KB trampoline in
   DOS memory (`ap_tramp.S`). It loads DOS/4GW's own GDT, the BSP's CR0/CR4
   and FPU/MXCSR settings, and calls the worker on its own stack.
2. **Choice of cores**: exactly one logical CPU per physical core,
   excluding the game's core and its Hyper-Threading sibling. The cores are
   addressed individually via CPUID leaf 0Bh topology; broadcast is the
   fallback. Sharing a physical core with the game slowed down the output
   measurably.
3. **Idle and shutdown**: unused APs halt. When the last worker ends, every
   AP receives INIT again and is back in its BIOS state. Nothing remains
   resident.
4. **Requirements**: ring 0 without paging (DOS/4GW without memory manager
   or with HIMEM only), a local APIC, flat code and data selectors.
5. **Ring 3 under a DPMI host** (SBEMU needs the game in ring 3 to trap its
   ports): the APs do not belong to the host and still start in ring 0,
   with their own flat GDT. They share the game's address space in one of
   two ways:
   - *paging* (HDPMI32i): the host emulates reading CR0/CR3/CR4, so the
     APs load its page directory. The trampoline page must be
     identity-mapped, which the OVL checks by walking the page tables
     through DPMI 0800h. Every AP reloads CR3 before its job.
   - *flat* (VDPMI): the host maps every linear address 1:1 to the same
     physical one. CR3 reads as 0 and DPMI 0800h returns physical addresses
     unchanged, so the APs run without paging and see the same memory.

   The other cores are chosen from the ACPI MADT (enabled local APICs) in
   every mode, ring 0 included. Firmware may own a CPU: CSMWrap, a legacy
   BIOS for UEFI-only machines, keeps one AP as its BIOS proxy and hides it
   from the MADT, so it never receives an IPI. CPUID topology and a
   broadcast are the fallbacks without an MADT.

   The local APIC is reached through DPMI 0800h (xAPIC mode only). Before
   an AP touches memory outside the trampoline, a guard compares a heap
   word and the first code bytes with the main core's view and halts the
   AP on a mismatch. Windows in enhanced mode (INT 2Fh 160Ah, CX = 3) is
   excluded. `GLIDULATOR_SMP` selects a path or turns this off.

Otherwise rendering stays on one core.

**Safety net**: if a worker raises a CPU exception or makes no progress for
2 s, the main core:
1. lets the other workers finish the queue,
2. redraws the lost worker's scanlines (skipping the triangle it stopped
   on),
3. reconciles the texture-cache reference counts,
4. continues on one core.

The faulting AP is identified through its stack address.

**Windows/Linux**: workers are threads. Windows 9x reports one core, so
none are started there.

## 6. Output

The rasterizer always renders RGB565 at the game's resolution. Output is
separate. `GLIDULATOR_DISPLAY` chooses the screen mode and
`GLIDULATOR_SCALE` the rectangle (`platform/common/output_rect.h`).

**DOS** (`platform/dos_ovl/video.c`):
1. VBE 2.0+ with linear framebuffer, 15/16/24/32 bpp. The smallest mode
   that fits wins; at equal size RGB565 (plain copy) is preferred, or
   32-bit when the video filter is on.
2. Fallback (or `GLIDULATOR_DISPLAY=vga`): VGA mode 13h, stretched to
   320x200 (4:3 like the monitor). Picking single pixels would sample the
   Voodoo's 4x4 dither irregularly (coarse stripes) and cutting bits loses
   what it encodes, so:
   - every target pixel averages the source area it covers, at least
     2x2 (any 2x2 window of the 4x4 Bayer pattern sums to nearly its
     mean): the undithered colour with about 8 bits per channel;
   - gamma on the average;
   - one ordered dither to a 6x7x6 palette whose levels are denser in the
     dark. The threshold matrix is fixed in screen coordinates, so the
     picture does not crawl (error diffusion would). Default: 16x16 blue
     noise (`tools/gen/bluenoise.py`, void-and-cluster), no second
     regular grid over the picture; Bayer 4x4 and rounding are options
     (`GLIDULATOR_VGA_DITHER`).
   - Per pixel: three table lookups (level below, fraction) and a compare
     with the threshold.
3. Otherwise no picture, but the game keeps running.

The BIOS access layer is a small function table (`vid_bios`). The logic is
therefore testable on Linux against a simulated BIOS.

**Write-combining (DOS)**: the OVL runs in ring 0 without paging, so the
memory type of the framebuffer depends only on the MTRRs. Firmware usually
marks the whole PCI window uncachable (UC), which makes framebuffer writes
very slow.
- `mtrr_plan.c` splits every UC range that overlaps the framebuffer into
  aligned pieces without it and adds one write-combining range.
- `wc_dos.c` writes the plan with caches disabled (CR0.CD, WBINVD) and
  restores the saved MTRRs on close.
- The planner is tested against real firmware layouts. It never changes the
  memory type outside the framebuffer.
- Measured on a Core i7-4770T at 800x600: output 24 ms -> 4 ms per frame.

**Windows** (`platform/win98/platform_win32.c`):
- GDI `SetDIBitsToDevice`/`StretchDIBits` into an owned, never-activated,
  click-through, topmost full-screen window. It adopts the game's
  foreground window as owner and re-asserts its position periodically.
- The display mode is switched to the game resolution (16-bit first, or
  32-bit with the video filter) and restored on close.
- Gamma is folded into the RGB565 -> screen conversion tables.
- The DLL allocates from its own heap (`HeapCreate`), not the game's
  process heap, so an overrun on either side cannot corrupt the other's
  heap blocks.
- `grSstSelect` restores the program's active window if it is minimized.
  The 3dfx MiniGL sizes the Glide screen and clip window from that
  window's client area right after this call. GLQuake in full screen
  minimizes itself when its own start-up dialog takes the focus, and the
  MiniGL would otherwise read 0 x 0 and clip everything away.

**LFB pointer lifetime**: on a Voodoo the `grLfbLock` pointer addresses the
board and stays valid, and some games keep writing through it after
`grLfbUnlock` or even after `grSstWinClose` (Uprising on exit). The colour/
depth buffer and the LFB shadow buffer are therefore kept across
`grSstWinClose`, reused by the next `grSstWinOpen` and released only when
the DLL/OVL is unloaded.

**Vsync (DOS)**: polls VGA status port 3DAh. If no retrace is seen within
100 ms it is disabled for the session. Page flipping is not used, so
tearing is possible.

## 7. Image quality

Order per frame: render -> FSAA resolve -> edge smoothing -> video filter
-> on-screen display -> output.

**FSAA** (`core/fsaa.c`):
- **Planes**: N complete planes (colour + depth) in the framebuffer
  memory; plane 0 is the normal buffer.
- **Drawing**: each triangle is rasterized N times with a sub-pixel offset
  added to its vertex registers. Start values and gradients refer to
  vertex A and move with it, so the rasterizer and JIT stay unchanged.
  Patterns as on the Voodoo 5: 2x diagonal (+-1/4 px), 4x rotated grid.
- **Resolve**: averages the planes per channel with an ordered-dither
  threshold. Identical planes resolve exactly to their value, so menus and
  HUDs drawn through the LFB stay pixel-exact.
- **LFB**: writes go to all planes, reads come from plane 0. Write locks go
  through a shadow buffer, and only changed pixels are propagated.
- **`auto`**: allocates 4 planes and switches between 1, 2 and 4 from the
  measured frame time (budget 1/60 s). It does nothing without a usable
  timer.

**Edge smoothing** (`GLIDULATOR_AA_POST`): a pixel whose depth differs from
a neighbour by more than 1/32 is mixed 1/4 with it. Pixels written through
the LFB are tracked in a bitmap and excluded.

**Voodoo video filter** (`core/vfilter.c`):
- The Voodoo Graphics' video output averaged neighbouring pixels within a
  scanline when they differed by less than a threshold. This removes the
  4x4 dither pattern but keeps edges.
- 3dfx's driver enabled it with threshold `0x080408`.
- The GL-1 reproduces it as in 86Box: per-channel lookup tables, four
  passes per line.
- The result has more than 16 bits per pixel and is output as XRGB8888.

**Start-up show** (`GLIDULATOR_SPLASH` for the logo, `GLIDULATOR_CRT` for
the monitor emulation, independent of each other; `core/splash.c`,
`crt.c`, `splash_orig.c`):
- **Monitor emulation** works on the finished RGB565 image right before
  output: vertical roll with a visible blanking bar, sideways line
  displacement, brightness overshoot, static. On the game's first frame
  after `grSstWinOpen` (or at the start of the logo) it plays blocking
  for 0.9 s, on `grSstWinClose` for 0.35 s on the last image.
- **Leaving the mode early**: some DOS programs switch to text mode
  themselves and print their exit text before `grSstWinClose`. Writing
  to the LFB would then garble that text (the LFB aliases the text
  planes), and restoring the mode would clear it. `vid_mode_lost` asks
  the BIOS (VBE `4F03h`, `0Fh` for mode 13h); if the mode is no longer
  ours, the sync-out is skipped and the mode is left alone.
- **Logos** draw through the public Glide functions, so FSAA and the
  video filter apply. Trace counting and frame capture are muted
  (`g_glide_mute`). Afterwards the framebuffer, texture memory and caches
  are cleared and the state saved before the animation is restored: the
  game sees exactly a fresh `grSstWinOpen`.
- **Timing**: fixed-rate loops paced by `platform_time_us`. Without a
  clock (DOS without TSC) they run unpaced instead of waiting forever.
- **Glidulator animation** (`gl1`, the default): generated 2D geometry on
  a 640x480 virtual screen, drawn back to front without depth buffer or
  blending: night sea, moon with halo, glittering reflection column, a
  silhouetted tentacle (tube along a curling centre line, per-point
  normals) with wave rings and its reflection, lightning, the chrome
  wordmark with a glint band in the vertex colours. Things fade in by
  mixing towards the colour behind them. Sliver triangles (tentacle tip)
  are skipped: they cover no pixel but produce gradients that overflow
  the rasterizer's 32-bit setup. Skipped without a clock.
- **Original 3dfx animation**: the model and textures are read from an
  original Glide 2.x binary, never shipped. The data block (vertex and
  face arrays of three objects, object tables, frame count, one matrix per
  frame and object, three 3DF textures) is found by internal consistency:
  pointer differences in the object tables must equal the array sizes.
  This works with absolute pointers (MSVC DLL) and unrelocated object
  offsets (Watcom OVL) alike and needs no fixed offsets. Every array is
  validated. Rendering follows the documented behaviour of 3dfx's splash:
  viewport, lighting, materials, highlight pass, projected shadow,
  fog-table fades, 30 frames per second.

## 8. Diagnostics

**Variants** are compile-time switches:
- `GLIDULATOR_TRACE`: logging, performance lines, first-call tracing.
- `GLIDULATOR_DEBUG`: adds crash handling, frame capture and verbose logs.
  It implies `GLIDULATOR_TRACE`.

The release build contains neither.

**Frame capture and replay**:
- The debug variant records frames: all clears and triangles with their
  registers, plus texture memory and tables.
- `glide_replay` re-renders them on Linux with the same core. It can list
  single triangles and compare render paths and thread counts.
- Limitation: recording starts with an empty depth buffer. Games that never
  clear depth (GLQuake's default `gl_ztrick 1`) cannot be replayed exactly.

**DOS crash handling (debug variant)**:
- While the display is open, the OVL installs its own IDT gates for #DE,
  #UD, #GP and #MC on the main core. #GP is told apart from IRQ 5 by the
  PIC's in-service register.
- It enables `CR4.MCE` on all cores.
- On a fault it logs registers, the code address and the machine-check
  banks of all cores, stops the other cores, restores write-combining and
  text mode and exits.
- `tools/dosvm/ovladdr.py` maps code addresses to functions with the
  matching map file.
- The log is flushed to disk for important lines and otherwise once per
  second.

## 9. Known limitations

- Static Glide: games that link Glide statically or program the hardware
  directly cannot be redirected.
- One TMU: there is no second texture unit and no alpha planes.
- `grLfbLock` with `GR_ORIGIN_LOWER_LEFT` returns an unflipped pointer.
- No vsync on Windows yet, and no page flipping on DOS.
- Under DOSBox, the emulated S3 BIOS shows mode 640x400x32 black. This is
  an emulator issue; it does not occur on real hardware without that mode.
