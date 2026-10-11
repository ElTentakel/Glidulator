# targets/glide_ovl: GLIDE2X.OVL

The DOS module: a DOS/4G LE DLL that games load through the extender
(`LINEXE_LOADMODULE`) and query by name (`_GRDRAWTRIANGLE@12`). Built with
Open Watcom (`make ovl`, `ovl-trace`, `ovl-debug`), module name and flags
patched by `tools/abi/lefix.py`.

- `dllmain.c`: `__DLLstart_` entry point, display open/close (VBE options,
  diagnostics hooks), `platform_blit`/`platform_blit32`.
- Platform layer: `platform/dos_ovl/` (DPMI memory and files, VBE/VGA
  output, write-combining, CPU cores, diagnostics).
- No C runtime startup: use only `platform_*` functions.

See [docs/technical-reference.md](../../docs/technical-reference.md),
sections 3 and 6.
