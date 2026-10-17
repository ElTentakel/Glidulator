# Glidulator GL-1 Compatibility List

*ElTentakel Graphics Systems, Publication No. ETGS-GL1-CL*

Tested with the release variant, unless noted otherwise. "Runs" means
rendered correctly through the whole demo or into actual gameplay. Most
tests ran in DOSBox (DOS) or Wine (Windows); titles marked HW were also
run on real hardware (Core i7-4770T, FreeDOS / Windows 98SE).

## DOS (`GLIDE2X.OVL`)

| Title | Type | Status | Notes |
|---|---|---|---|
| Nature (Vertigo, 1997) | demo | **runs** (HW) | 512x384; loading screen through the LFB; multi-core and all image options tested on HW. Setup: `2`, `n n n n`, `1`, `1`, then `1`. |
| Dimension (gOds, 1998) | demo | **runs** (HW) | 640x480, needs >40 MB memory; clipped geometry through `gu*WithClip`. |
| Shortcut (Buckshag, 1998) | demo | **runs** | `.3DF` textures, table fog. |
| Trip (KFMF, 1997) | demo | **runs** | Particles, environment mapping. |
| Fucktory (Capsicum Tincture, 1998) | demo | **runs** | YIQ/NCC textures, `GetState`/`SetState`. |
| Grand Theft Auto (DMA Design, 1997) | game demo | **runs** | `guTex*` texture management, LFB locks. |
| Burnout: Championship Drag Racing (Bethesda, 1998) | game demo | **runs** | Fog, lines, motion blur through the LFB. |
| Tomb Raider (Core Design, 1996), Voodoo Rush patch `TUMBRUSH.EXE` | game | **runs** (QEMU/KVM, FreeDOS) | Needs `GLIDULATOR_BOARD=rush`, otherwise it quits after the intro videos. Title menu and demo mode tested with the full CD in the drive (LaserLok: DOSBox fails with error 607). |
| Archimedean Dynasty (Massive, 1996) | game demo, 3Dfx patch | menus tested | Mission start needs mouse operation, not automated. |
| Yume 2 (inf, 1998) | demo | not tested | Crashes in DOSBox before the first Glide call (not Glidulator-related). |

## Windows (`glide2x.dll`)

| Title | Type | Status | Notes |
|---|---|---|---|
| GLQuake 0.97 + 3dfx MiniGL, Quake 1.06 shareware | game | **runs** (HW) | `timedemo demo1`: 62 fps (3 workers, Wine), 31 fps with FSAA 2x. Keep the 3dfx `opengl32.dll` in the game directory. On Windows 98 the picture froze when the first demo started (0.9.0, GLQuake minimizes itself behind its own progress dialog); fixed in 0.9.1. |
| Nature95 (Vertigo, 1997) | demo | **runs** | |
| Formula 1 (Psygnosis) | game demo | **runs** | Into the race, cockpit view. |
| Operation 3DFX '98 (7 entries) | demo contest | **all run** | No Reality, Chrome II (3 parts), Visions, Planet Cornball (3dfx MiniGL), Daggmask, Oblivion, Scenarios. |
| Incubation (Blue Byte) | game demo | menus tested | Needs installation; title and menus run through Glide. |
| Uprising (3DO) | game demo | **runs** (Windows 98) | Crashed on exit: it writes through the LFB pointer after closing Glide. Fixed (the buffers outlive `grSstWinClose`), retest on hardware pending. The game itself does not start under Wine. |

## Not supported

- **Statically linked Glide**: the Voodoo Graphics versions of Tomb
  Raider (`TOMB3DFX.EXE`) and Carmageddon link Glide into the executable
  and program the hardware directly. There is no OVL/DLL to replace. Tomb
  Raider's Voodoo Rush patch loads Glide and runs, see above.
- **Descent and Descent 2 3Dfx patches**: they patch the full versions
  only; the shareware/demo data is not accepted.
- **Glide 3.x**: games that need `glide3x.dll` are out of scope.

## Known issues in 0.9.1

This is a preview release. Known problems, in the order we intend to
look at them:

- **Uprising**: the crash on exit is fixed, but the fix has not been
  confirmed on real hardware yet.
- **Rasterizer**: single pixels can stay open on the shared edges of a
  triangle fan (seen with 24 triangles around one centre point).
- **Glide functions without effect**: Glide's own anti-aliased primitives
  (`grAA*`), `guMP*` (multi-pass texturing helpers) and
  `guTexCreateColorMipMap`. `grLfbLock` with `GR_ORIGIN_LOWER_LEFT`
  returns a pointer for upper-left origin. No tested title is affected.
- **DOS with a memory manager**: some machines with a lot of RAM refuse
  DOS/4GW games with `DOS/16M error: [40]` (not Glidulator-specific); boot
  without a memory manager.
- **SBEMU with VDPMI** (pre-release DPMI host): Dimension hangs, Nature
  stays black after its countdown. Use HDPMI32i.
- **UEFI machines with CSMWrap**: one laptop (Ryzen 5 5500U) crashed in
  SBEMU's VDPMI build while loading, and Dimension hung under HDPMI32i.
  Not reproducible elsewhere so far.
- **Windows 95/98/ME** run Glide games on one CPU core; this is a limit of
  the operating system.

## Remarks for testers

Developers of DPMI hosts, sound emulators and BIOS layers: see
[interoperability.md](interoperability.md) for what the DOS module relies
on and the known issues involving other projects.

- Run the Debug variant when something fails and keep `GLIDULAT.LOG`. Its
  frame captures can be replayed on a development machine.
- GLQuake runs in full screen without `-window` (its self-minimizing
  start-up is handled by the DLL).
- **Sound emulation with SBEMU** (Sound Blaster on PCI/HDA cards): works.
  SBEMU needs JEMMEX and the DPMI host HDPMI32i (or its VDPMI
  pre-release); DOS/4GW games then run under that host in ring 3. The OVL
  still renders on several cores (it starts them on the host's address
  space, see `GLIDULATOR_SMP`); under HDPMI32i with the JIT, under VDPMI
  with the C path. Write-combining stays off. On real hardware (Core
  i7-4770T, SBEMU 1.0.0-beta.6 with HDPMI32i) Nature and Dimension run
  with 3 render workers and the JIT; whether sound is audible depends on
  SBEMU and the sound chip's output routing, not on Glidulator. DOS/32A
  games need `JEMMEX NOVCPI` after SBEMU (SBEMU's documentation).
