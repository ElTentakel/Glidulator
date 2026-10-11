# Attribution: LE tools

`lefix.py` and `ledump.py` are taken unchanged from
[ranulphus/MGA-Glide](https://github.com/ranulphus/MGA-Glide)
(`tools/abi/`, `master` of 2026-10-05), MIT licensed (Copyright (c) 2026 The
Nulph).

`wlink` writes the module name of `GLIDE2X.OVL` in upper case; the 3dfx
runtime uses `glide2x` and module flags `0x8200`. `lefix.py` patches both in
the linked file (same length, no layout change); `ledump.py` is the minimal
LE parser it needs. MGA-Glide's `docs/loader.md` and `docs/toolchain.md`
(MIT) were the basis for understanding the OVL format (DOS/4G LE DLL,
`__DLLstart_` entry point, `__stdcall` export names).
