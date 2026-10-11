# Attribution: LE loader

`leload.c`/`leload.h` are taken unchanged from
[ranulphus/MGA-Glide](https://github.com/ranulphus/MGA-Glide)
(`tests/shim/`, `master` of 2026-10-05), MIT licensed.

A clean-room loader for DOS/4G LE DLLs: it parses the MZ/LE headers, loads
the objects, applies internal fixups and resolves exports by name. File and
memory access go through callbacks, so it also runs outside DOS.
`tools/leload_demo/` uses it to load the real, Watcom-built `GLIDE2X.OVL`
in a 32-bit Linux process and call its exports (`make ovl-demo`), the same
code path a DOS game takes.
