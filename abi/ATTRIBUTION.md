# Attribution: Glide 2.x ABI description

`glide2x.api`, `types.h.frag` and `constants.tsv` are taken from
[ranulphus/MGA-Glide](https://github.com/ranulphus/MGA-Glide) (`abi/`,
`master` of 2026-10-05). `types.h.frag` is extended by the `PciRegister`
type of 3dfx's fxpci library (exported by glide2x.dll); everything else is
unchanged.

- License: MIT (Copyright (c) 2026 The Nulph), compatible with Glidulator's
  GPL.
- Contents: the complete clean-room reconstruction of the Glide 2.x API
  (names, signatures, types, structure layouts verified against real games
  and the 3dfx retail runtime) and all named constants.
- The `status`/`used-by` columns describe MGA-Glide's own implementation.
  `tools/abi/gen_glide2x.py` uses only names, signatures and tiers;
  which functions Glidulator implements is listed there (`REAL_IMPL`).

Glide is a trademark of its owner.

# Glide 3.x ABI description

`glide3x.api` lists the 93 functions of 3dfx's Glide 3.x runtime for the
Voodoo2 (names and signatures only), machine-extracted from the public
header `glide.h`/`glideutl.h` of the 3dfx Glide 3 source release
(`glide3x/cvg/glide3/src`, as mirrored at github.com/sezero/glide). The
numeric Glide 3 constants Glidulator needs are written as `G3_*` facts in
`core/glide3_abi.c`; no header text is reproduced. Glidulator's
implementation of these functions is its own.
