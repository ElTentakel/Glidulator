# Glidulator GL-1 Technical Reference

*ElTentakel Graphics Systems, Publication No. ETGS-GL1-TR*

This document describes how the GL-1 works and why it is built the way it
is. It records final design decisions only.

## 1. Overview

```
Game
  |  Glide 2.x calls (grSstWinOpen, grDrawTriangle, ...)
  v
targets/glide_ovl (DOS, GLIDE2X.OVL)      targets/glide_dll (Windows, glide2x.dll, glide3x.dll)
  |  exports, entry point; no own state
  v
core/glide3_abi.c  (glide3x.dll only)      Glide 3.x calls -> Glide 2.x calls
  |
  v
core/                                      portable, no OS knowledge
  glide_*.c       Glide API -> Voodoo registers ("register driver")
  render_mt.c     triangle queue for render workers
  rasterizer/     Voodoo Graphics rasterizer and texture unit (PCem/86Box), x86 JIT
  fsaa.c          sub-pixel planes, resolve, edge smoothing
  vfilter.c       Voodoo video output filter
  osd.c           output timing, on-screen display
  |  platform_* calls (core/include/glidulator/platform.h)
  v
platform/dos_ovl   Open Watcom: DPMI, VBE/VGA, MTRR write-combining, CPU cores
platform/win98     mingw-w64: GDI output, threads; vxd/: GLSMP.VXD (Open Watcom)
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
  It was written by Sarah Walker for PCem and ported from 86Box, which
  emulates the chip register-accurately.

This makes behaviour questions answerable by reference: what does 3dfx's
Glide 2.x source write, and what does the chip do with it. The 3dfx source
release is the reference for every API detail.

Notable consequences:
- Vertex coordinates are 12.4 fixed point and wrap like on the chip. This
  is why games add a "snap bias" to coordinates, and why the `gu*WithClip`
  functions really clip.
- `grSstQueryHardware` reports a Voodoo Graphics with 4 MB texture memory
  per TMU under Windows (2 MB under DOS, the common board), two TMUs under
  Windows and one under DOS (`GLIDULATOR_TMUS=1|2` overrides). With 2 MB, Unreal-engine games replace most
  of their textures every frame. Games quit if no board
  is found.
- With two TMUs, TMU1's output is TMU0's "other" input, as on the board.
  The state functions behave like 3dfx's Glide for both units: textures
  split into even and odd levels (`GR_MIPMAPLEVELMASK_EVEN/ODD`) are laid
  out per level parity, `lodBlend` enables trilinear filtering, and TMU1
  takes TMU0's S/T and W unless the `GR_STWHINT_ST_DIFF_TMU1` /
  `W_DIFF_TMU1` hints say otherwise. With one TMU, even/odd downloads keep
  the unsplit layout and `lodBlend` is ignored.
- Decoded textures are cached per TMU: up to 1024 textures in a pool that
  grows by need in 1 MB pieces up to 24 MB (DOS 8 MB). Most DOS games need
  0.5-2.5 MB of decoded textures per frame (Burnout 4.5 MB), so they keep
  1-3 MB. Each texture gets as much of the pool as its mip levels need;
  the least recently used texture of the same size class is replaced, and
  if a frame needs more of one class than the pool holds, the pool is
  emptied once and refilled. Unreal-engine games use 100-250 textures per
  frame; the earlier 32 full-size slots decoded nearly all of them again
  in every frame.
- Alpha buffer: without depth buffering the aux buffer holds alpha, as on
  Voodoo Graphics (`FBZ_ALPHA_ENABLE`, set by `grDepthBufferMode`).
  Blending reads destination alpha from it, `grColorMask(.., FXTRUE)`
  writes alpha, `grBufferClear` clears it with its alpha argument. C path
  and recompiler both implement it (bit-identical).
- Gamma defaults to 1.3, the value of later 3dfx drivers. Configurable via
  `SST_GAMMA`.

ABI: all 132 Glide 2.x functions and the 14 `pci*` functions of 3dfx's
runtime are exported under their original decorated names. Most are
implemented; the rest are rarely used and exported as harmless stubs:
- antialiased primitives `grAA*`
- `guMP*`, `guMovie*`
- `grSplash`
- RLE16 textures

Declarations are generated from a clean-room API description
(`abi/glide2x.api`, `tools/abi/gen_glide2x.py`).

**Glide 3.x** (`glide3x.dll`) is the same core built with
`-DGLIDULATOR_GLIDE3`: a generated block renames the Glide 2 functions to
`g2_*`, and `core/glide3_abi.c` implements the 93 Glide 3 exports as
`g3_*` on top of them (`abi/glide3x.api`, `tools/abi/gen_glide3x.py`).
Trace, capture and replay therefore work unchanged on the Glide 2 calls.
- Vertices: `grVertexLayout` builds a table of offsets; each vertex of a
  draw call is copied into a Glide 2 `GrVertex` (window coordinates,
  packed or float colour, S/T and Q per TMU). TMU1 without its own S/T
  takes TMU0's, as the hardware does; the S/T/W hints follow the layout.
- Level of detail and aspect ratio use Glide 3's numbering (log2) and are
  converted for the core; fog modes, tables and texture bases likewise.
- `grGet`/`grGetString` answer like 3dfx's Voodoo2 driver: hardware
  "Voodoo2", version "3.03", two TMUs (`GLIDULATOR_TMUS=1` overrides), no
  extensions. `grQueryResolutions` offers 320x200 to 800x600.
- Not supported yet: clip coordinates (`grCoordinateSpace(GR_CLIP_COORDS)`,
  `grViewport` and `grDepthRange` are only stored), `grLoadGammaTable`,
  stipple patterns, and `grAADrawTriangle` draws without edge
  anti-aliasing.

## 3. Module formats

- **`GLIDE2X.OVL` is not an overlay but a DOS/4G LE DLL**. Games load it
  through the extender (`LINEXE_LOADMODULE`) and resolve functions by name
  (`_GRDRAWTRIANGLE@12`). Only Open Watcom's `wlink` produces this format,
  so the OVL is built with Open Watcom. `tools/abi/lefix.py` sets module
  name and flags the way games expect.
- The OVL has no C runtime startup. Memory comes from DPMI (`INT 31h`
  0501h) per allocation, and nothing in the OVL relies on CRT
  initialisation.
- **`glide2x.dll`** and **`glide3x.dll`** are built with mingw-w64 for
  Windows 4.0. Exports carry 3dfx's exact names and mixed casing
  (`targets/glide_dll/glide2x.def`, `glide3x.def`), libgcc is linked
  statically, and only system DLLs are imported
  (`KERNEL32`, `USER32`, `GDI32`, `msvcrt`).

## 4. Rendering paths

Two independent axes, chosen at `grSstWinOpen` (`core/renderer.c`):

| Axis | Choices |
|---|---|
| Pixel pipeline | **C** interpreter (always available), **JIT**: x86 code generated per render-state combination with SSE2 (32-bit builds only) |
| Threads | 1, or 1-8 render workers (the game's core drawing too with up to 7) |

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
worker draws every triangle, but only its own scanlines. A table of 840
entries (divisible by 1 to 8), repeated down the screen, names the owner
of each line. Every
worker times its drawing with the time stamp counter; after each frame the
lines move halfway towards equal drawing times, so a worker that shares a
core with another gets fewer lines. Equal shares give `y % n == i`, as in
86Box. The picture does not depend on the split. The game continues while
workers draw.

With up to seven workers the game's core is a drawer too (at most eight drawers), with its own
lines. It draws them only when it would otherwise wait: at a
synchronisation point, when the queue is full, and while it waits for a
worker. Its share follows a simple model: the game's core needs its own
game time G plus its drawing, a worker only its drawing, and every drawer
should finish at the same time. When G alone is longer than the workers'
drawing, the game's core gets no lines (it joins again from 5 % of the
lines and leaves below 1.7 %). It waits only at
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
   measurably. If the BIOS option "CPUID Limit" hides leaves 4 and 0Bh,
   Intel CPUs from Nehalem on are assumed to reserve one SMT bit in the
   APIC ID (they do so even with Hyper-Threading off), so the siblings are
   still recognised. `GLIDULATOR_HT=1` (experimental) targets every logical
   CPU; workers then go to one thread per other core first, then to those
   cores' siblings, and to the game core's sibling last.
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

**Windows NT/Linux**: workers are threads. There `GLIDULATOR_THREADS=auto`
starts at most four: the OS counts every logical CPU, and with a spinning
worker on each of them any other load preempts one and the frame waits
for it. Under DOS and with `GLSMP.VXD` the cores belong to Glidulator
alone, so `auto` uses up to eight.

**Multi-core under Windows 9x** (`platform/win98/vxd/glsmp.asm`, caller
`platform/win98/platform_win32.c`): Windows 95/98/ME schedule one core and
know nothing of the others. `glide2x.dll` loads the dynamic VxD
`GLSMP.VXD` from its own directory (`CreateFile("\\.\GLSMP.VXD")` with
`FILE_FLAG_DELETE_ON_CLOSE`; nothing is installed) at the first core-count
query and talks to it through `DeviceIoControl`. A VxD runs in ring 0, so
it starts the APs itself, as the OVL does under DOS:

1. **Address space**: the start call runs in the game's context. The VxD
   builds the APs' page directory from the live one (PDEs 1-511 are the
   game's private arena, 512-1023 the shared/system arena) with its own
   identity map for 0-4 MB (the trampoline). Every buffer a worker touches
   is locked with `_LinPageLock`: the DLL image, `msvcrt.dll`, and every
   `platform_alloc`/`platform_alloc_exec` block (the DLL registers them all
   and locks new ones at once). Locked pages keep their PTEs, so the APs'
   TLBs, which Windows never flushes, stay valid. A lock also adds page
   tables that appeared since the start to the APs' directory (only
   not-present entries, which no TLB caches).
2. **Rule for the core**: memory the workers can reach, even through stale
   pointers, is allocated and freed only while no worker runs.
   `grSstWinOpen` therefore starts the workers after every buffer exists. A
   waiting worker executes speculatively with the previous session's
   parameters; with buffers mapped anew under a running worker its TLB
   kept the old frames on real hardware (wrong pictures from the second
   session on, page faults in the third).
3. **Start**: INIT and two Startup IPIs (local APIC switched on only for
   each IPI, with interrupts off: Windows 98 runs with it off) to a
   trampoline page below 1 MB (`aptramp.S`). It loads Windows' GDT, the
   VxD's own IDT and the main core's CR0/CR4, enables paging and jumps to
   the VxD. Each AP takes a 64 KB stack from the VxD, clears CR0.TS and
   polls its job slot.
4. **Choice of cores**: as under DOS, one AP per physical core (CPUID
   topology, including the "CPUID Limit" rule); siblings of the main core and of already used cores park at
   once. All APs are started by broadcast. With `GLIDULATOR_HT=1` every AP
   takes a slot, and the DLL hands out its workers in the same order as
   under DOS (from the APIC IDs the VxD reports per slot).
5. **Jobs**: the DLL posts `fn(arg)` with its x87 control word and MXCSR;
   the AP reloads CR3, loads them and calls the worker (cdecl, flat
   selectors, so the ring-3 addresses are valid; FS is not the TIB, so
   worker code uses no SEH/TLS, as in the OVL). A CPU exception lands in the
   VxD's IDT, which records vector, EIP and CR2 and parks the AP; the DLL
   reports it to the safety net below.
6. **Stop**: on the last worker's end, on closing the handle and when
   Windows tears down a killed process (VWIN32 sends the close before it
   frees the process's memory): the APs leave their loop, receive INIT and
   then one more Startup IPI into a hlt loop of the BIOS ROM, so they answer
   SMIs (a core in wait-for-SIPI does not join the SMI rendezvous; on a
   Core i7-4770T that made USB-legacy keyboard access under DOS 20 times
   slower). Locks are released in the locking process's context only.
7. **Refused**: no MSRs, local APIC in x2APIC mode (its MMIO interface is
   off), not on the boot core, APIC above 4 GB, or no AP answering: one
   core, as without the file.

The VxD is hand-written for Open Watcom `wasm`/`wlink` (`form windows vxd
dynamic`); test programs: `tools/win98vxd/`.

## 6. Output

The rasterizer always renders RGB565 at the game's resolution. Output is
separate. `GLIDULATOR_DISPLAY` chooses the screen mode and
`GLIDULATOR_SCALE` the rectangle (`platform/common/output_rect.h`).

**DOS** (`platform/dos_ovl/video.c`):
1. VBE 2.0+ with linear framebuffer, 15/16/24/32 bpp. The smallest mode
   that fits wins; at equal size RGB565 (plain copy) is preferred, or
   32-bit when the video filter is on. Each source line is converted once
   through a table of all 65536 RGB565 values in the target format, gamma
   included (rebuilt when the format or the gamma changes), then copied
   into video memory line by line.
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
- Firmware on machines with much RAM spends registers on ranges above
  4 GB (i7-4790 with 32 GB: 12 of 10 registers needed). Without PAE
  nothing above 4 GB is reachable, so when the plan does not fit those
  ranges are left out while the game runs (6 registers there); the saved
  layout comes back on close.
- Measured at 800x600: Core i7-4770T output 24 ms -> 4 ms per frame;
  Core i7-4790 (32 GB, ranges above 4 GB left out) 17.1 ms -> 2.8 ms.
- Windows 98 (`wc_win9x.c`) uses the same planner from the DLL through a
  ring-0 call gate. GLQuake on an i7-4790 with VBEMP: 37.6 -> 56.3 fps
  (GDI copy 9-10 ms -> 0.1 ms per frame).
- Ring 3 under HDPMI32i (needed by SBEMU): `r3_dos.c` reaches ring 0
  through a temporary call gate in the host's GDT. The MTRR procedure
  through it (cache off, WBINVD, MTRRs, under the host's paging) froze an
  i7-4790 in about one call of three, so ring 3 uses PAT instead: with
  paging the page table entry selects a PAT entry, and PAT WC wins over
  MTRR UC. `wc_dos.c` points the host's entries of the framebuffer
  mapping at the unused PAT entry 5 and sets that entry to WC (one WRMSR
  plus a TLB flush through the gate). Not under VDPMI (no page tables to
  find the GDT). Redguard on an i7-4790 (SBEMU, 640x480): 35.9 -> 72.0
  fps, output 9.4 -> 0.4 ms per frame; Nature 42 fps / 17 ms output
  before.

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
- **Glide 3 splash module** (Windows `glide3x.dll` only): if
  `GLIDULATOR_SPLASH_FILE` names a file starting with `3dfxSpl`
  (3dfx's `3dfxSpl3.dll`, "3Dfx Splash Version 1.2"), `grSstWinOpen` loads
  it like 3dfx's Glide 3 did and calls `_fxSplashInit@24(hWnd, w, h,
  nColBuffers, nAuxBuffers, colorFormat)`, `_fxSplash@20(0, 0, w, h, 0)`
  and `_fxSplashShutdown@0`. The module imports 42 functions from
  `glide3x.dll`, all of which Glidulator exports, so it draws through
  Glidulator. It refuses fewer than two colour buffers or no aux buffer;
  Glidulator always has both, so at least 2 and 1 are passed. Glide state
  is saved with `grGlideGetState` before and restored afterwards, and the
  framebuffer and texture memory are cleared as after the other logos.
  If the module fails to load or initialise, the Glidulator animation
  plays.

## 8. Diagnostics

**Variants** are compile-time switches:
- `GLIDULATOR_TRACE`: logging, performance lines, first-call tracing.
- `GLIDULATOR_DEBUG`: adds crash handling, frame capture and verbose logs.
  It implies `GLIDULATOR_TRACE`.

The release build contains neither.

**Frame capture and replay**:
- The debug variant records frames: all clears and triangles with their
  registers, plus texture memory and tables. Since format version 4 also
  the game's state and texture calls (blend, alpha test, depth mask,
  combine, texture source and downloads) in order between the triangles.
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

## 9. Where the time goes

Measured on a Core i7-4790 (4 cores + HT) unless noted; "replay" means
`glide_replay` on recorded frames under Linux (32-bit, JIT, one core),
which isolates the renderer from the game.

**Work per frame, in order of cost:**

1. **Pixel pipeline** (render workers, or the game's core with
   `GLIDULATOR_THREADS=1`). Compute bound, not memory bound: IPC ~2, L1
   misses and branch misses ~1 %. Profile of the JIT path (replay, GLQuake
   with two TMUs / Nature):
   - generated pixel code 40 % / 36 %. Inside it: texel fetch 60 % (TMU1
     35 %, TMU0 25 %) / 45 %, colour combine 16 %, fog, alpha, dither and
     write 16 %, depth test 9-15 %;
   - perspective-correct texture coordinates (`vj_persp_fetch`, two
     64x64-bit multiplications and a division per pixel and TMU) 32 % / 15 %;
   - triangle setup and level of detail per 8-pixel span part
     (`voodoo_triangle`, two double divisions and a `log2` per part and
     TMU) 19 % / 33 %. Skipped where it cannot change the picture (one mip
     level, or the whole triangle provably on the finest level).
2. **Triangle setup on the game's core**: plane equations for colour,
   depth, W and S/T/W per TMU, then the render queue (256 entries).
3. **Texture work on the game's core**: decoding a texture into the
   cache on first use (P8, 16-bit formats to 32-bit ARGB), copying
   uploads into texture memory and invalidating cached textures they
   overlap. Unreal-engine games load hundreds of small lightmaps; since
   the pooled cache this costs 0.1-1.7 ms per frame in Unreal
   Tournament's intro (was 4-8 ms).
4. **Output** (game's core): converting the 16-bit frame for the screen,
   optional filters and OSD, then the copy to video memory -- by GDI in
   the display driver under Windows, directly to the linear framebuffer
   under DOS. Cost depends almost only on the video memory type (below).

**Where the game waits** (`glide_mt_idle`): at `grBufferSwap` (all
triangles of the frame drawn), `grBufferClear`, every `grLfbLock` and
`grLfbWriteRegion`, when the texture cache must reuse an entry a worker
still draws with, and during vsync (`GLIDULATOR_VSYNC`). Games that read
or write the framebuffer every frame (motion blur, software cursors)
therefore lose the overlap between game logic and rendering.

**What each optimisation brings** (frames per second, higher is better):

| Optimisation | Windows 98 | DOS |
|---|---|---|
| Write-combining for the framebuffer (`GLIDULATOR_WC`, default on; VBEMP and other generic drivers) | GLQuake 640x480: 37.6 -> 56 fps; GDI copy 9-10 ms -> 0.1 ms per frame | Nature, 800x600 output: output 17.1 -> 2.8 ms per frame, 26 -> 49 fps at the same spot (at 640x480 about two thirds of these output times) |
| JIT instead of C pixel path (needs SSE2) | renderer 1.8-1.9 times as fast (replay) | same; needs ring 0 or a DPMI host that enables SSE |
| Multi-core (`GLIDULATOR_THREADS=auto`) | GLQuake 640x480 (0.9.1): 1 core 15.4 fps, 4 cores 38.8 fps; Windows 98 needs `GLSMP.VXD` | Nature 800x600: 2 cores 75 -> 98 fps, 4 cores 125 -> 143 fps with the game's core drawing too |
| Hyper-threading (`GLIDULATOR_HT=1`) | no gain | slower (4C+HT: 114 vs 88 fps) |
| Two TMUs (default under Windows) | GLQuake 640x480 timedemo: 76.6 -> 93.8 fps; Unreal Tournament intro 640x480: 40-46 -> 51-55 fps; Undying 800x600: 26 -> 37 fps | not measured; costs about 3 MB more memory (up to 10 MB) |
| Level of detail only where needed (0.9.5) | GLQuake 640x480: 67.5 -> 76.0 fps | Nature: 104 -> 117 fps |
| 4 MB texture memory per TMU (Windows) | Undying: uploads 12 MB -> 0.4 MB per frame, 22 -> 26 fps | -- (2 MB) |
| Pooled texture cache | Undying: 37 -> 42 fps; UT intro decode 4-8 -> 0.1-1.7 ms | smaller memory footprint |
| Ring 0 instead of ring 3 under DOS | -- | Nature: render 9.1 -> 7.1 ms; before write-combining through the call gate, ring 3 also had no write-combining (42 fps vs 128 fps) |

Image options cost extra: the Voodoo video filter about 0.3 ms per frame
of output at 640x480 (GLQuake 69.1 -> 64.8 fps), FSAA 2x halves the frame
rate (GLQuake 38.8 -> 19.4 fps), and vsync caps at the refresh rate.

## 10. Known limitations

- Static Glide: games that link Glide statically or program the hardware
  directly cannot be redirected.
- Two TMUs by default under Windows, one under DOS (`GLIDULATOR_TMUS`).
- Glide 3: no clip coordinates yet (see section 2); no DOS version of the
  Glide 3 layer.
- `grLfbLock` with `GR_ORIGIN_LOWER_LEFT` returns an unflipped pointer.
- No vsync on Windows yet, and no page flipping on DOS.
- Under DOSBox, the emulated S3 BIOS shows mode 640x400x32 black. This is
  an emulator issue; it does not occur on real hardware without that mode.
