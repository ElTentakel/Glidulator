# Glidulator GL-1 Compatibility List

*ElTentakel Graphics Systems, Publication No. ETGS-GL1-CL*

Tested with the release variant, unless noted otherwise. "Runs" means
rendered correctly through the whole demo or into actual gameplay. Most
tests ran in DOSBox (DOS) or Wine (Windows); titles marked HW were also
run on real hardware (Core i7-4770T or Core i7-4790, FreeDOS / Windows 98SE).

## DOS (`GLIDE2X.OVL`)

| Title | Type | Status | Notes |
|---|---|---|---|
| Nature (Vertigo, 1997) | demo | **runs** (HW) | 512x384; loading screen through the LFB; multi-core and all image options tested on HW. Setup: `2`, `n n n n`, `1`, `1`, then `1`. |
| Dimension (gOds, 1998) | demo | **runs** (HW) | 640x480, needs >40 MB memory; clipped geometry through `gu*WithClip`. |
| Shortcut (Buckshag, 1998) | demo | **runs** (HW) | `.3DF` textures, table fog. 65-78 fps on a Core i7-4790. After it ends, its closing text and the DOS prompt can appear in a garbled text mode (also with 0.9.5); a reboot restores it. |
| Trip (KFMF, 1997) | demo | **runs** (HW) | Particles, environment mapping. Setup: `3` (3Dfx), `0` (640x480), `0` (no sound). Core i7-4790: 36 fps with `GLIDULATOR_FSAA=2` (the demo's own frame rate). |
| Fucktory (Capsicum Tincture, 1998) | demo | **runs** (HW) | YIQ/NCC textures, `GetState`/`SetState`. The long milky-white passages are the demo's own effect. |
| Grand Theft Auto (DMA Design, 1997) | game demo | **runs** (HW) | `guTex*` texture management, LFB locks. Start `DEMOFX.EXE`; Core i7-4790: about 23 fps in Liberty City, the game's own frame rate. |
| Burnout: Championship Drag Racing (Bethesda, 1998) | game demo | **runs** (HW) | Fog, lines, motion blur through the LFB. |
| Tomb Raider (Core Design, 1996), Voodoo Rush patch `TUMBRUSH.EXE` | game | **runs** (QEMU/KVM, FreeDOS) | Needs `GLIDULATOR_BOARD=rush`, otherwise it quits after the intro videos. Title menu and demo mode tested with the full CD in the drive (LaserLok: DOSBox fails with error 607). |
| Pył (Optimus Nexus, 1998) | game demo | **runs** (HW) | Start with `PYLDEMO -V1` (3Dfx; the Windows setup program writes the same choice). Needs about 32 MB for itself on top of Glidulator; before 0.9.3 loading a level failed with `CH_C3DFX[80]` (out of memory: its DOS/4GW 1.97 reported the same 20 MB free with 63 and 127 MB of RAM). Real hardware: first level on a Core i7-4770T (no memory manager) and a Core i7-4790 (no memory manager, and HDPMI32i), about 30 fps on the 4790 without write-combining, 61 fps with it (also under SBEMU/HDPMI32i since write-combining works in ring 3). With EMM386 but without HDPMI32i it crashes on one tester's machine (see Known issues). In QEMU the menu ignores mouse clicks. |
| The Elder Scrolls Adventures: Redguard (Bethesda, 1998), GOG release, `RGFX.EXE` | game | **runs** (HW) | DOS/4GW: start under HDPMI32i (MS-DOS 7.1 with HIMEM, or FreeDOS with SBEMU). Menus with page turning, intro movies and gameplay; Core i7-4790: 70-72 fps in town (25 fps before write-combining worked under SBEMU). Movies: GOG's batch files mount them as drive `R:` (Windows NT syntax); under DOS map the `MOVIES` directory to a free drive with `SUBST` (for example `E:`, within `LASTDRIVE`) and set `animation_drive=E:\` in `SYSTEM.INI`. Sound effects through SBEMU (Sound Blaster 16 in `SOUND\SETSOUND`); the music is CD audio and needs the game CD in a drive. |
| Lands of Lore: Guardians of Destiny (Westwood, 1997), GOG release, `LOLG.EXE` | game | **runs** (QEMU/KVM, FreeDOS, emulated Sound Blaster 16) | Its own DOS/4GW Professional runs with virtual memory limited to 24 MB and leaves about 3 MB for the Glide driver: set `DOS4GVM=VirtualMemory:ON,VirtualSize:48M,SwapFileName:lolgswap.swp,SwapMin:20M,SwapInc:0` before starting, otherwise `grSstWinOpen` fails (black screen). 3Dfx must be switched on in `OPTIONS.INI` (`[Acceleration]`, `Acceleration_Toggle=On`, or in the game's options). Sprites use the alpha buffer (destination alpha), supported since 0.9.6. The game needs a working Sound Blaster and a mouse driver; its HMI sound library does not accept SBEMU 1.0.0-beta.6 (all IRQ/DMA settings, 8- and 16-bit drivers, `JEMMEX NOVCPI`: "Sound card not set up correctly"; also in QEMU), so no HW test yet. Without a DPMI host its virtual memory turns on paging in ring 0 and Glidulator uses one core; under HDPMI32i (with a real Sound Blaster) it uses all cores. |
| Archimedean Dynasty (Massive, 1996) | game demo, 3Dfx patch | **runs** (HW) | Start `AD3DFX.EXE`, click through the info and licence pages, then "Instant Action". Menus are VESA; the missions use Glide. Core i7-4790: mission "Moghs" at 35-41 fps with `GLIDULATOR_FSAA=2`. |
| Yume 2 (inf, 1998) | demo | not tested | Packed executable: crashes in DOSBox and stops on a Core i7-4790 with `DOS/4GW fatal error (1004)` before the first Glide call (not Glidulator-related). |

## Windows (`glide2x.dll`)

| Title | Type | Status | Notes |
|---|---|---|---|
| GLQuake 0.97 + 3dfx MiniGL, Quake 1.06 shareware | game | **runs** (HW) | `timedemo demo1`: 94.3 fps on a Core i7-4790 (Windows 98, `GLSMP.VXD`, two TMUs, 0.9.6); 62 fps (3 workers, Wine), 31 fps with FSAA 2x. Keep the 3dfx `opengl32.dll` in the game directory. On Windows 98 the picture froze when the first demo started (0.9.0, GLQuake minimizes itself behind its own progress dialog); fixed in 0.9.1. Windows 98 on HW with `GLSMP.VXD`: `timedemo demo1` 38.8 fps on 4 cores (3 workers) against 15.4 fps on one; demo2 37.2, demo3 37.8, FSAA 2x 19.4 fps. With `GLIDULATOR_TMUS=2` the MiniGL offers multitexturing ("Multitexture extensions found") and GLQuake draws walls and lightmaps in one pass: 55 -> 66 fps (QEMU/KVM, four cores), 76.6 -> 93.8 fps on an i7-4790 (Windows 98, `GLSMP.VXD`). |
| Half-Life 1.1.1.0 (Valve, 1998), OpenGL with the 3Dfx mini driver | game | **runs** (HW) | Start with `-gl`. Windows 98 on HW with `GLSMP.VXD`: the picture froze after one or two minutes ("not responding"); fixed in 0.9.4 (a render worker's triangle was overwritten while it drew it). Tested on an i7-4790: tram ride and four minutes of play, 30-70 fps; also with `GLIDULATOR_TMUS=2`. |
| Nature95 (Vertigo, 1997) | demo | **runs** (HW) | Windows 98 on HW with `GLSMP.VXD` (multi-core): 84-175 fps on a Core i7-4790. Quit with `Q`. |
| Formula 1 (Psygnosis) | game demo | **runs** (HW) | Into the race, cockpit view; Windows 98 on HW with `GLSMP.VXD` (multi-core): 60-64 fps in the race on a Core i7-4790. |
| Operation 3DFX '98 (7 entries) | demo contest | **all run** | No Reality, Chrome II (3 parts), Visions, Planet Cornball (3dfx MiniGL), Daggmask, Oblivion, Scenarios. On a Core i7-4790 under Windows 98: Visions, Oblivion, Scenarios, Daggmask and No Reality run; No Reality only at 6-20 fps (the same with 0.9.5). Chrome II's parts are console programs: Windows 98 opens a DOS window for them, which the generic VBEMP display driver cannot show (garbled screen until reboot). Under Wine they run. |
| Incubation (Blue Byte) | game demo | menus tested | Needs installation; title and menus run through Glide. |
| Unreal Tournament (Epic, 1999), demo v348 and GOTY v436 (GOG) | game | **runs** (HW) | Glide renderer (`GlideDrv`), which needs `glide2x.dll` (not `glide3x.dll`) in `System\`. Without it the game reports "Can't find file for package 'GlideDrv'" and falls back to software rendering. GOTY intro on a Core i7-4790 under Windows 98 with `GLSMP.VXD`: about 60 fps, 640x480. Demo under Wine: Deathmatch with bots, 64 fps with one TMU, 75 fps with `GLIDULATOR_TMUS=2`. With two texture units (the Windows default) lightmapped walls are drawn about half as bright as with one: the game multiplies texture and lightmap once in its one-pass path but twice (blending) in its two-pass path. Whether a real Voodoo2 looks the same is not yet checked; `GLIDULATOR_TMUS=1` gives the brighter picture. |
| Clive Barker's Undying (EA, 2001), CD v1.0 | game | **runs** (HW) | Unreal-engine Glide renderer, needs `glide2x.dll` in `System\`. In the first-start dialog choose "Show all devices" and "3dfx Glide for Windows". Needs two texture units (the Windows default; do not set `GLIDULATOR_TMUS=1`): with one texture unit the game draws see-through objects (ship, trees, the house on the hill in the intro) and then their lightmap as a second pass over the whole polygon, which leaves a dark or black rectangle around them (the register state matches what the game asks for, so a real single-TMU board should show the same). With two units it combines both in one pass: correct picture, and faster. Core i7-4790, Windows 98, `GLSMP.VXD`, 800x600: menus 150+ fps; first level (courtyard) about 42 fps with two units (37 before the larger texture cache), 26 fps with one (17 fps before the faster texture uploads and 4 MB texture memory). |
| Unreal Gold (Epic, 1998), v226 (GOG) | game | **runs** (HW) | Same Glide renderer and the same `glide2x.dll` requirement as Unreal Tournament. Castle flyby on a Core i7-4790 under Windows 98 with `GLSMP.VXD`: 59-84 fps, 640x480 (0.9.5: 30-55 fps). |
| Uprising (3DO) | game demo | **runs** (HW) | Crashed on exit: it writes through the LFB pointer after closing Glide. Fixed (the buffers outlive `grSstWinClose`), confirmed on a Core i7-4790 under Windows 98: mission 1 ("Play Demo") at 130-150 fps, clean exit. "Quickstart" does not start (also not under Wine). Its menus use 8-bit DirectDraw, which the generic VBEMP display driver shows with wrong colours. The game itself does not start under Wine. |

## Windows, Glide 3 (`glide3x.dll`)

Part of the package since 0.9.6. It reports a Voodoo2 with two TMUs. Put
it next to the game's renderer DLL, like `glide2x.dll`.

| Title | Type | Status | Notes |
|---|---|---|---|
| Need for Speed: Porsche Unleashed (EA, 2000) | game and demo | **runs** (HW) | Renderer `Drivers\voodoo2z.dll`; select "voodoo2" as Thrash driver. Race on a Core i7-4790 under Windows 98 with `GLSMP.VXD`: 75-81 fps; replays work. Table fog verified on the Alps track. |
| Operation Flashpoint (Codemasters, 2001), demo | game demo | **runs** (VM) | Glide 3.03 renderer, 800x600: menus and the "Ambush" mission. Uses detail textures and fog from the iterated depth. |
| Diablo II (Blizzard, 2000), shareware v1.04 | game demo | **runs** (HW) | Put `glide3x.dll` next to `Diablo II.exe` and start it with `-3dfx`. Menus (800x600), character creation and the Rogue Encampment (rain, shadows, lighting). Core i7-4790 under Windows 98 with `GLSMP.VXD`: the game's own frame limit (25 fps in town, up to 50 fps), four cores. |
| Slave Zero (Infogrames, 1999), demo | game demo | **runs** (HW) | Set `Renderer DLL=3dfx.dll` in `SZoptions.ini`. Intro, menus and the first mission; Core i7-4790 under Windows 98 with `GLSMP.VXD`: about 57 fps in the mission, 640x480. Under Wine the demo crashes while loading the level, also with its Direct3D renderer. |

## Not supported

- **Statically linked Glide**: the Voodoo Graphics versions of Tomb
  Raider (`TOMB3DFX.EXE`) and Carmageddon link Glide into the executable
  and program the hardware directly. There is no OVL/DLL to replace. Tomb
  Raider's Voodoo Rush patch loads Glide and runs, see above.
- **Descent and Descent 2 3Dfx patches**: they patch the full versions
  only; the shareware/demo data is not accepted.
- **Direct3D games**: Glidulator replaces Glide only.

## Known issues in 0.9.6

This is a preview release. Known problems, in the order we intend to
look at them:

- **Glide 3** (`glide3x.dll`): clip coordinates
  (`grCoordinateSpace(GR_CLIP_COORDS)` with `grViewport`/`grDepthRange`)
  are not supported yet, `grLoadGammaTable` and stipple patterns are
  ignored, and `guGammaCorrectionRGB` uses the average of its three
  values. No tested game is affected.
- **Unreal Tournament with two texture units** (the Windows default)
  draws lightmapped walls darker than with one (see above). Which picture
  matches a real Voodoo2 is not checked yet; `GLIDULATOR_TMUS=1` gives the
  brighter one.
- **Shortcut** (DOS) can leave a garbled text mode after it ends (also
  with 0.9.5); a reboot restores the screen.
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
- **Pył with EMM386 but without HDPMI32i** crashes while loading a level
  on one tester's machine (HIMEM + EMM386, DOS/4GW directly). Without
  EMM386, or with HDPMI32i, it loads. Cause not known yet; please report
  the error text and a trace log if you see it.
- **Windows 95/98/ME** run Glide games on one CPU core unless `GLSMP.VXD`
  lies next to `glide2x.dll`. Multi-core under Windows 9x is tested in a
  virtual machine and on two real PCs (Core i7-4770T, Core i7-4790), so
  far Intel only; if a game misbehaves with it, try `GLIDULATOR_SMP=off`.
- **Windows 98, game ended through the task list**: write-combining
  stays on until the next reboot (the next start reports `already on`).
  Harmless; quit games normally.
- **`GLIDULATOR_HT=1`** is experimental. On four cores with
  Hyper-Threading it is about as fast as the default (up to 0.9.4:
  slower) and can help in games with much drawing.
- **More than four render workers** (CPUs with more than four cores) are
  tested in virtual machines only. Reports from 6- and 8-core machines,
  also AMD, are welcome: OSD values with `GLIDULATOR_THREADS=4` against
  `auto`.
- **`GLIDULATOR_TMUS=2` under DOS** needs about 3 MB more memory (up to
  10 MB in games with many textures); games that leave little free memory
  may fail to load. Under Windows two units are the default;
  `GLIDULATOR_TMUS=1` switches back to one.

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
  with the C path. Write-combining works under HDPMI32i (through a call
  gate to ring 0), not under VDPMI. On real hardware (Core
  i7-4770T, SBEMU 1.0.0-beta.6 with HDPMI32i) Nature and Dimension run
  with 3 render workers and the JIT; whether sound is audible depends on
  SBEMU and the sound chip's output routing, not on Glidulator. DOS/32A
  games need `JEMMEX NOVCPI` after SBEMU (SBEMU's documentation). If SBEMU
  stops with `Sound card IRQ 5 conflict with options /i5`, the sound chip
  itself uses IRQ 5 (Intel HDA on some boards): start it with `/I7` and set
  the game's sound setup to IRQ 7.
