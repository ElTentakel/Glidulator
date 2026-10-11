# Attribution: Voodoo rasterizer

The files `vid_voodoo_*.c/h` and `vid_voodoo_dither.h` in this directory are
ported from [86Box](https://github.com/86Box/86Box) (`src/video/
vid_voodoo_render.c`, `vid_voodoo_texture.c` and their headers, `master` of
2026-10-05; the x86 code generator `vid_voodoo_codegen_x86.h` from `master`
of 2026-10-08).

- Copyright 2008-2020 Sarah Walker and the 86Box contributors (see file
  headers).
- License: GNU GPL, version 2 or later. Glidulator is distributed under the
  same terms.

The Voodoo video output filter in `core/vfilter.c` re-implements
`voodoo_generate_filter_v1`/`voodoo_filterline_v1` from 86Box's
`vid_voodoo_display.c` (same authors and license).

## Changes against 86Box

**Removed**: 86Box's own threading and device plumbing:
- `voodoo_queue_triangle`, the render threads and their wake/wait helpers.
  Glidulator calls `voodoo_triangle()` directly; multi-core rendering is
  done by `core/render_mt.c` on top of it.
- PCI/SLI setup and register write handling.

**Added**:
- A declaration of `voodoo_triangle()`.
- `vid_voodoo_tables.c` with `voodoo_init_color_tables()`, the unchanged
  table-filling code from `voodoo_init()`.
- `voodoo_update_ncc()` from `vid_voodoo.c`. It is called by
  `grTexDownloadTable` instead of a register write.

**Changed**:
- `voodoo_use_texture()` includes a checksum of the selected NCC table in
  the cache key for YIQ formats, as the palette is for P8.
- Scanline distribution `y % render_threads`, so any worker count works.
- The texture cache waits for workers instead of failing when full.
- Per-pixel statistics counters removed (`VOODOO_STAT`).
- Logging and timers mapped to Glidulator (`voodoo_compat.h`); 86Box
  thread/event types are placeholder typedefs.

**JIT** (`vid_voodoo_codegen_x86.h`, 32-bit x86 only):
- No SSE intrinsics (Open Watcom): constant tables in self-aligned memory.
- Executable memory from `platform_alloc_exec()`; code blocks survive
  `grSstWinOpen`.
- cdecl call convention (`VOODOO_JIT_CALL`), also from Watcom code.
- LOD fix from 86Box 3a94359.
- Perspective division through `vj_persp_fetch()`, so JIT and C path are
  bit-identical.
- One dead line removed.

Triangle setup, scanline filling, texture sampling, dithering, depth/alpha
tests and LOD selection are otherwise unchanged.

## Note: `voodoo->params` vs. `params`

`voodoo_use_texture()` and parts of `voodoo_triangle()` read both the
`params` argument and the `params` embedded in `voodoo_t`. In 86Box both are
the same object. Glidulator passes a local copy, so `core/glide_abi.c` syncs
`g_voodoo.params` before calling into the rasterizer.
