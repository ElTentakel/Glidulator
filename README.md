# Glidulator GL-1
**The Voodoo-compatible 3D accelerator that fits in a single file.**
No add-in board. No pass-through cable. No jumpers. Every core a tentacle.

---

**ElTentakel Graphics Systems** proudly presents the **Glidulator GL-1**, a
software implementation of the 3dfx Voodoo Graphics accelerator and its
Glide 2.x programming interface for **MS-DOS** and **Windows 98**.

Drop one file next to your favourite late-90s game, and it renders through
Glidulator instead of a Voodoo card: texture-mapped, perspective-correct,
Z-buffered, fogged and alpha-blended, register-for-register the way the
real chip did it. Where the original needed a dedicated board, the GL-1
puts the idle cores of a modern CPU to work: each additional core
becomes another tentacle of the rasterizer.

> ElTentakel Graphics Systems is a fictional company. Any resemblance to
> actual 90s hardware vendors, living or bankrupt, is entirely intentional
> and affectionate. See [Trademarks](#trademarks).

## Product highlights

- **Drop-in replacement**: `GLIDE2X.OVL` for DOS games (DOS/4G LE DLL) and
  `glide2x.dll` for Windows 9x games. All 132 Glide 2.x entry points and the
  14 `pci*` helpers of the original runtime are exported with their original
  names; nothing is installed and nothing in the system is changed
  permanently.
- **Faithful rendering**: Glide calls program the same Voodoo registers the
  original driver did, and a register-accurate rasterizer (ported from
  86Box) draws the triangles. Gamma like the Voodoo RAMDAC, 4x4 dithering
  like the chip.
- **Multi-core rendering**: up to four render workers, also under plain
  DOS, where Glidulator starts the additional CPU cores itself.
- **x86 JIT**: per render state, the pixel pipeline is compiled to SSE2
  machine code at run time (about twice as fast as the C path).
- **Image quality options** (all off by default):
  - FSAA with rotated-grid sub-pixel offsets like the Voodoo 5
    (2x, 4x or automatic).
  - Cheap edge smoothing after rendering.
  - The Voodoo Graphics video output filter for the authentic
    "Voodoo look".
- **Start-up show**: the Glidulator animation (a tentacle rising from a
  moonlit sea) or 3dfx's own spinning
  logo read from an original Glide file you already have, and, on its
  own or combined, an analog monitor locking onto the Voodoo signal.
- **Modern displays**: aspect-correct scaling, any display mode, optional
  on-screen display with frame rate and render times.
- **Diagnostics**: trace and debug variants with performance logging,
  frame capture and replay, and crash handling on real hardware.

## Supported systems

|            | DOS                                            | Windows                                              |
|------------|------------------------------------------------|------------------------------------------------------|
| File       | `GLIDE2X.OVL`                                  | `glide2x.dll`                                        |
| Host       | MS-DOS / FreeDOS with DOS/4GW games            | Windows 95/98/ME (also runs under Wine)              |
| Display    | VBE 2.0 linear framebuffer (15/16/24/32 bpp)   | GDI, own full-screen window                          |
|            | fallback VGA mode 13h                          |                                                      |
| CPU        | 386+; JIT needs SSE2 (quad core recommended)   | 486+; JIT needs SSE2                                 |
| Multi-core | yes, without memory manager or with HIMEM only | Windows 9x uses one core; NT/XP and Wine use threads |

## Quick start

The current version is **0.9.0, a preview release**: the tested titles
run, a few known issues remain (see
[Known issues](docs/compatibility.md#known-issues-in-090)). Download the
package `glidulator-0.9.0.zip` from the
[Releases](https://github.com/ElTentakel/Glidulator/releases) page.

1. Pick the variant you need. Each comes as an OVL and as a DLL:

   | Variant | Folder       | Use                                          |
   |---------|--------------|----------------------------------------------|
   | Release | `BIN\`       | Play games.                                  |
   | Trace   | `BIN\TRACE\` | Measure performance (writes `GLIDULAT.LOG`). |
   | Debug   | `BIN\DEBUG\` | Diagnose problems.                           |

2. Copy `GLIDE2X.OVL` (DOS) or `glide2x.dll` (Windows) into the game's
   directory, replacing the one shipped with the game.
3. Start the game and select its 3Dfx/Glide renderer.
4. Optional: copy the example `GLIDULAT.CFG` from the package next to it
   and remove the `#` in front of the settings you want to change. See the
   [User Guide](docs/user-guide.md#3-settings).

## Compatibility

Tested titles are listed in [docs/compatibility.md](docs/compatibility.md),
among them:

- **DOS**: Nature, Dimension, Shortcut, Trip, Fucktory, Grand Theft Auto,
  Burnout.
- **Windows**: GLQuake (with the original 3dfx MiniGL), Nature95,
  Formula 1, Operation 3DFX '98.

Games that link Glide statically instead of loading the OVL/DLL (e.g. the
3Dfx versions of Tomb Raider and Carmageddon) cannot be supported this way.

## Documentation

| Document                         | Contents                                              |
|----------------------------------|-------------------------------------------------------|
| [User Guide](docs/user-guide.md) | Installation, all settings, variants, troubleshooting |
| [Compatibility](docs/compatibility.md) | Tested software and known limitations           |
| [Technical Reference](docs/technical-reference.md) | Architecture and design decisions   |
| [Development](docs/development.md) | Building, tests and tools                           |
| [Interoperability](docs/interoperability.md) | For developers of DPMI hosts, SBEMU, BIOS layers: what the DOS module relies on |

## Building

Requirements:
- **Core and tests**: GCC on Linux.
- **Windows DLL**: mingw-w64 (`i686-w64-mingw32-gcc`).
- **DOS OVL**: Open Watcom v2.

Details are in [docs/development.md](docs/development.md).

```
make test                            # reference build and unit/render tests (ASan/UBSan)
make ovl-all                         # GLIDE2X.OVL: release, trace and debug variants
make win98 win98-trace win98-debug   # glide2x.dll variants
make dosbox-test                     # load the OVL in DOSBox and check the picture
make dist                            # release package in build/dist/
```

## License and credits

Glidulator is free software under the **GNU General Public License,
version 2 or later** (see `COPYING`), as required by the rasterizer it is
built on. It stands on the shoulders of:

- **PCem** (Sarah Walker), as further developed in **86Box** (86Box contributors), GPL:
  Voodoo rasterizer, texture unit, x86 JIT and video filter. See
  [core/rasterizer/ATTRIBUTION.md](core/rasterizer/ATTRIBUTION.md).
- **MGA-Glide** (ranulphus, MIT): clean-room Glide 2.x ABI description and
  the LE loader used in tests. See [abi/ATTRIBUTION.md](abi/ATTRIBUTION.md)
  and [tools/leload/ATTRIBUTION.md](tools/leload/ATTRIBUTION.md).
- The 3dfx Glide 2.x source release (sezero/glide), used as the reference
  for driver behaviour.

## Trademarks

3dfx, Voodoo and Glide are trademarks of their respective owners.
Glidulator is not affiliated with, endorsed by or derived from software of
3dfx Interactive or its successors. "ElTentakel Graphics Systems" is the
tongue-in-cheek alter ego of its author, ElTentakel, and not a real
company. No tentacles were harmed in the making of this software.
