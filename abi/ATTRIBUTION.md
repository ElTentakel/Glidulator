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
