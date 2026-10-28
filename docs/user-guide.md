# Glidulator GL-1 User Guide

*ElTentakel Graphics Systems, Publication No. ETGS-GL1-UG*

Thank you for choosing the Glidulator GL-1. Unlike other 3D accelerators
of its time, the GL-1 requires no screwdriver. Please read this guide
before calling technical support; there is no technical support.

## 1. What is in the box

| File | Purpose |
|---|---|
| `BIN\GLIDE2X.OVL` | Glide 2.x for DOS games (DOS/4G LE DLL, loaded by the game) |
| `BIN\glide2x.dll` | Glide 2.x for Windows 9x games |
| `BIN\GLSMP.VXD` | Multi-core rendering under Windows 95/98/ME: loaded by `glide2x.dll` while the game runs (optional) |
| `BIN\TRACE\`, `BIN\DEBUG\` | The same files in the Trace and Debug variants |
| `GLIDULAT.CFG` | Example settings file: every setting, commented out, with its default |
| `README.TXT` | Short introduction and known issues |
| `MANUAL.TXT`, `COMPAT.TXT` | This guide and the compatibility list as plain text |
| `COPYING.TXT` | The GNU General Public License |

The source code is at <https://github.com/ElTentakel/Glidulator>.

Every Glide file exists in three variants, identical in features and
output:

| Variant | Writes `GLIDULAT.LOG` | Extra behaviour | Use it to |
|---|---|---|---|
| Release | no | none | play |
| Trace | performance line every 100 frames, first use of each Glide function, chosen renderer, display mode, write-combining, CPU cores | none | measure |
| Debug | everything of Trace, plus texture/triangle details | own crash handler (DOS), machine-check reporting, frame capture for replay | diagnose |

## 2. Installation

### DOS

1. Copy `GLIDE2X.OVL` into the game directory, replacing the game's own
   copy (games look in their own directory first, then along `PATH`).
2. Start the game with its 3Dfx option.

Requirements and recommendations:
- A VESA BIOS with VBE 2.0 and linear framebuffer gives the best picture.
  Without it, Glidulator falls back to VGA mode 13h (320x200, 256 colours,
  dithered; see `GLIDULATOR_VGA_DITHER`).
- **For multi-core rendering and the JIT**, boot DOS without a memory
  manager, or with HIMEM only. Under EMM386 or other VCPI managers and in
  the DOS box of Windows, the OVL does not run in ring 0. It then renders
  on one core with the C path; everything else works.
  After "Restart in MS-DOS mode" from Windows 98 (no memory manager), the
  OVL switches the local APIC that Windows left off back on for the time
  the extra cores run, so multi-core works there too.
- Some DOS/4GW games abort with
  `DOS/16M error: [40] not enough available extended memory (XMIN)` when a
  memory manager is loaded on machines with a lot of RAM. Boot without one.

### Windows 95/98/ME

1. Copy `glide2x.dll` into the game directory (next to the `.exe`, or the
   directory of the game's renderer DLL), replacing the game's own copy.
2. For multi-core rendering, copy `GLSMP.VXD` into the same directory.
3. Start the game with its 3Dfx option.

Glidulator opens its own full-screen output window above the game's window
and switches the display mode for the duration of the game (see
`GLIDULATOR_DISPLAY`).

Windows 95/98/ME schedule one CPU core only. With `GLSMP.VXD` next to the
DLL, Glidulator starts the other cores itself and runs its render workers
there, as under DOS: one worker per physical core (Hyper-Threading siblings
stay idle), up to four. The VxD is loaded dynamically when the game first
uses Glide and unloaded when it ends, also when it is killed; nothing is
installed and the registry is not touched. Without the file, or with
`GLIDULATOR_SMP=off`, rendering stays on one core. The Trace log shows
`Multi-core: Windows 9x VxD up, ...` when it is in use. The CPU's local
APIC must be in xAPIC mode (every multi-core PC of the Windows 98 era and
later boots in it unless the firmware selects x2APIC).

For games using the 3dfx MiniGL (`opengl32.dll` for Voodoo), keep that DLL.
It calls `glide2x.dll`.

## 3. Settings

Glidulator works without any settings. To change something, copy the
example `GLIDULAT.CFG` from the package next to the game's `GLIDE2X.OVL`
or `glide2x.dll` and remove the `#` in front of the lines you want to
change.

**Where settings come from**

- **DOS**: `GLIDULAT.CFG` in the current directory, that is, the
  directory the game was started from (normally the game directory).
- **Windows**: an environment variable of the same name first
  (`SET GLIDULATOR_OSD=1` before starting the game), then `GLIDULAT.CFG`
  in the current directory. A shortcut's "Start in" field sets that
  directory.

Settings are read when the game opens its Glide window. Change them
before starting the game.

**File format**

```
# comment
GLIDULATOR_OSD=1
glidulator_fsaa = 2      anything after the value is ignored
```

- One `NAME=value` per line. Spaces around `=` are allowed.
- Lines starting with `#` are ignored. Names are not case-sensitive.
- A value ends at the first space or tab, so a path in a value must not
  contain spaces (use the short 8.3 name on Windows, e.g.
  `C:\PROGRA~1\...`).
- Only the first 8 KB of the file are read; the example uses less than
  half of that.
- Missing, empty or unknown values keep the default. A misspelled name is
  ignored silently: check the OSD or the Trace log if a setting seems to
  have no effect.

**Common recipes**

| Goal | Settings |
|---|---|
| See frame rate and render path | `GLIDULATOR_OSD=1` |
| 16:9 monitor that stretches 4:3 modes | `GLIDULATOR_DISPLAY=1280x720` (or `1920x1080`) |
| Fill the whole screen | `GLIDULATOR_SCALE=stretch` |
| DOS game runs too fast | `GLIDULATOR_VSYNC=1` |
| Smoother edges | `GLIDULATOR_FSAA=auto` |
| Visible bands where textures switch to a smaller mipmap | `GLIDULATOR_LOD_DITHER=1` |
| The 1997 look | `GLIDULATOR_VOODOO_FILTER=1`, `GLIDULATOR_SPLASH=original` with `GLIDULATOR_SPLASH_FILE`, `GLIDULATOR_CRT=1` |
| Voodoo Rush version of a game quits after the intro | `GLIDULATOR_BOARD=rush` |
| Rule out multi-core or JIT problems | `GLIDULATOR_THREADS=1`, `GLIDULATOR_RENDERER=c` |
| Brighter or darker picture | `SST_GAMMA=1.0` (darker) ... `1.7` (brighter) |

The tables below list every setting. "DOS only" settings are ignored by
the Windows DLL.

### Rendering

| Setting | Values | Default | Effect |
|---|---|---|---|
| `GLIDULATOR_RENDERER` | `auto`, `c`, `jit` | `auto` | Pixel pipeline. `auto` picks the JIT when the CPU has SSE2 and the system saves SSE registers, otherwise the C path. Both give identical pictures. |
| `GLIDULATOR_THREADS` | `auto`, `1` ... `4` | `auto` | Render workers on other cores. `auto` = cores - 1, at most 4; `1` = render on the game's core (under Windows 9x the VxD is then not loaded). Windows 95/98/ME need `GLSMP.VXD` next to the DLL. |
| `GLIDULATOR_SMP` | `auto`, `ring0`, `paging`, `flat`, `off` | `auto` | DOS only: how the OVL starts the other cores. `auto` uses plain DOS/4GW (ring 0) as before and, under a DPMI host such as HDPMI32i or VDPMI (SBEMU), detects which of the two host paths fits. `ring0` = only the plain-DOS path, `paging` = host with page tables (HDPMI32i), `flat` = host that maps memory 1:1 (VDPMI), `off` = one core. Under Windows 95/98/ME only `off` matters: it keeps `GLSMP.VXD` unloaded (one core); every other value uses it when present. |
| `GLIDULATOR_VSYNC` | `auto`, `0`, `1` | `auto` | DOS only: wait for vertical retrace before showing a frame. `auto` follows the game's `grBufferSwap` interval, `1` always waits, `0` never. Keeps games that time their logic per frame at the original speed. |
| `GLIDULATOR_BOARD` | `voodoo`, `rush` | `voodoo` | Which 3dfx board Glidulator reports to the game. `voodoo` = Voodoo Graphics. `rush` = Voodoo Rush, for Rush patches that refuse to run on anything else, such as Tomb Raider's `TUMBRUSH.EXE` (it quits right after the intro videos without a message). Rendering is the same; leave it at `voodoo` for all other games. |
| `SST_GAMMA` | number | `1.3` | Output gamma like the Voodoo RAMDAC (the variable name of 3dfx's driver). `1.0` is neutral; higher values are brighter. |

### Display

| Setting | Values | Default | Effect |
|---|---|---|---|
| `GLIDULATOR_DISPLAY` | `glide`, `desktop`, `WxH`, `WxHxBPP`; DOS also `vga` | `glide` | Screen mode. `glide` = the game's resolution (or the smallest mode it fits into). `desktop` = do not switch (Windows). `1280x720` etc. for a fixed mode, e.g. on 16:9 monitors. `vga` = VGA mode 13h only (plain VGA, no VESA driver needed). |
| `GLIDULATOR_VGA_DITHER` | `blue`, `bayer`, `off` | `blue` | DOS mode 13h (256 colours): how the picture is reduced. Every pixel first averages the area it covers, which removes the Voodoo's own dither pattern. `blue` then dithers with a fine, irregular pattern that stays still from frame to frame, `bayer` with the classic regular pattern, `off` rounds to the nearest colour (visible bands). |
| `GLIDULATOR_SCALE` | `aspect`, `stretch`, `integer`, `none` | `aspect` | How the picture fills the screen. `aspect` keeps 4:3 for every game resolution, as Voodoo boards displayed it, with black borders. |
| `GLIDULATOR_WC` | `0`, `1` | `1` | Enable write-combining for the video memory while the game runs. This is several times faster on real hardware. The original MTRRs are restored on exit. Windows 95/98/ME: only with generic display drivers (VBEMP and the like); real drivers set it themselves. |
| `GLIDULATOR_OSD` | `0`, `1` | `0` | On-screen display: frames per second, render and output time per frame, render path, cores in use/available (for example `1/2 CORES`: the game's core plus render workers, out of the cores Glidulator can use; under DOS one per physical core, under Windows NT/2000/XP every logical processor, under Windows 9x always 1), how the other cores run (`SMP RING0`, `PAGING` or `FLAT` under DOS, see `GLIDULATOR_SMP`; `SMP GLSMP` under Windows 9x with `GLSMP.VXD`; `SMP THREADS` under Windows NT/2000/XP and Wine; `SMP OFF` = one core), resolutions and active image options. It is never visible to the game. On a screen mode smaller than the game's picture (DOS mode 13h) it is drawn larger so it stays readable. |

### Image quality

All image options are off by default; with them off, the picture is
bit-identical to plain rendering.

| Setting | Values | Default | Effect |
|---|---|---|---|
| `GLIDULATOR_FSAA` | `0`, `2`, `4`, `auto` | `0` | Full-scene anti-aliasing like the Voodoo 5: every triangle is drawn 2 or 4 times with sub-pixel offsets (rotated grid) and averaged. It costs about 2x or 4x the render time. Menus, HUDs and videos drawn by the game directly into the framebuffer stay sharp. `auto` switches between off, 2x and 4x to stay at 60 frames per second. |
| `GLIDULATOR_AA_POST` | `0`, `1` | `0` | Cheap edge smoothing after rendering: pixels at depth discontinuities are blended with their neighbour. It costs about 1-2 ms per frame. It does not smooth alpha-tested edges, and pixels the game wrote directly are left untouched. |
| `GLIDULATOR_LOD_DITHER` | `auto`, `0`, `1` | `auto` | Mipmap dithering, as the Voodoo board does it: the edge between two mipmap levels becomes a 4x4 dither pattern instead of a visible band. `auto` dithers when the game asks for it (`GR_MIPMAP_NEAREST_DITHER` with `GR_HINT_ALLOW_MIPMAP_DITHER`), `1` always (like the "mipmap dithering" option of 3dfx's drivers), `0` never. Perspective-corrected textures only. |
| `GLIDULATOR_VOODOO_FILTER` | `0`, `1`, `RRGGBB` | `0` | The video output filter of the Voodoo Graphics ("Voodoo look"). Neighbouring pixels that differ only by dithering are averaged, edges stay. `1` uses 3dfx's default threshold `080408`; a hex value sets your own. Output then uses 24/32-bit screen modes. It costs about 4-5 ms per frame at 640x480. |

The options can be combined. The on-screen display is not filtered.

### Start-up show

| Setting | Values | Default | Effect |
|---|---|---|---|
| `GLIDULATOR_SPLASH` | `gl1`, `off`, `original` | `gl1` | Logo after the first `grSstWinOpen`. `gl1`: the Glidulator animation (4.1 s): a moonlit night sea, a tentacle rising out of the water in front of the moon, and the chrome GLIDULATOR wordmark with a glint running along it. Skipped under DOS without a clock (no TSC, e.g. DOSBox). `original`: 3dfx's own spinning-logo animation (2.5 s), read from an original Glide 2.x file (see below). (The older value `crt` still means `GLIDULATOR_CRT=1` without a logo.) |
| `GLIDULATOR_SCREENSHOT` | `0`, `1`, `2` | `1` | DOS: Print Screen saves the next frame as `SHOT0000.BMP`, `SHOT0001.BMP`, ... in the current directory (24-bit BMP in the game's render resolution, with gamma, without OSD, scaling or video filter). `2`: the same, but with the OSD (`GLIDULATOR_OSD=1`) in the picture, for example to share frame rates. While the game runs, the OVL puts a small handler in front of the game's keyboard handler; it only looks at the key and passes everything on. `0` leaves the keyboard alone. Under Windows, Print Screen stays Windows' own (desktop to the clipboard). |
| `GLIDULATOR_CRT` | `0`, `1` | `0` | Monitor effect: every switch to the Voodoo picture looks like an analog monitor locking onto a new signal (the picture rolls, lines bend, brightness settles, a little static), and switching back loses sync the same way. Works with or without a logo. Under DOS the switch back is skipped if the program has already returned to text mode itself. |
| `GLIDULATOR_SPLASH_FILE` | path without spaces | - | An original Voodoo Graphics `GLIDE2X.OVL` or `glide2x.dll` (as shipped with many games; either file works on both systems). Glidulator reads only the animation's model and textures from it and draws them itself; nothing from the file is executed. If the file is missing or contains no splash data, the Glidulator animation plays instead. |

Like 3dfx's driver, the logo plays once per program start and the game
then finds Glide in its initial state. The start-up show is never part of
frame captures or trace counts.

### Example

```
# GLIDULAT.CFG: 16:9 monitor, OSD, the full 1997 experience
GLIDULATOR_OSD=1
GLIDULATOR_DISPLAY=1280x720
GLIDULATOR_FSAA=auto
GLIDULATOR_VOODOO_FILTER=1
GLIDULATOR_SPLASH=original
GLIDULATOR_SPLASH_FILE=C:\GAMES\POD\GLIDE2X.OVL
GLIDULATOR_CRT=1
```

## 4. Trace and debug variants

The Trace and Debug variants append to `GLIDULAT.LOG` in the current
directory. Typical lines:

```
Glidulator GL-1 0.9.3 (trace)
Renderer: jit, Threads 3, CPU: MMX SSE SSE2
Video mode: 800 x 600
Frame: 60.1 fps, game+render 12345 us, output 2400 us per frame
```

The last line shows frames per second, the time from one output to the
next request (game logic plus rendering) and the output time. When it is
significant, it also shows the time spent in texture downloads and direct
framebuffer writes.

The Debug variant additionally:
- **Records frames** for replay on a development machine (`CAPnn.BIN`,
  `TEXnn.BIN`, `TABnn.BIN`). A file `CAPTURE.CFG` with `first interval
  count` controls which frames are recorded; `0 1 0` disables recording.
- **DOS only: handles crashes itself.** On a CPU exception or machine
  check it logs registers, the code address and the machine-check banks of
  all cores, stops the extra cores, restores write-combining and text mode
  and exits.

## 5. Troubleshooting

| Symptom | Remedy |
|---|---|
| The game says no 3Dfx card was found | Make sure the game uses our `GLIDE2X.OVL`/`glide2x.dll` (game directory) and its 3Dfx option. Statically linked Glide games cannot use Glidulator. |
| DOS: `DOS/16M error: [40]` | Boot without memory manager (see Installation). |
| DOS: only one core used (OSD `SMP OFF`), OSD shows `C` instead of `JIT` | A memory manager or the Windows DOS box is active (no ring 0). Boot plain DOS. |
| Windows 9x: only one core used (OSD `1/1 CORE`, `SMP OFF`) | `GLSMP.VXD` must lie next to `glide2x.dll`. The Trace log's `Multi-core:` line gives the reason otherwise (e.g. x2APIC mode). |
| Windows 9x: problems that go away with one core | Set `GLIDULATOR_SMP=off` or remove `GLSMP.VXD`, and report it with the Trace log. |
| Picture too slow | Check the OSD. With FSAA or the video filter on, turn them off. Under DOS, make sure write-combining is active (Trace log "Write-Combining"). |
| Black borders | Expected with `GLIDULATOR_SCALE=aspect` on non-4:3 screens. Use `stretch` if you prefer. |
| Game runs too fast | Set `GLIDULATOR_VSYNC=1` (DOS). |
| A setting has no effect | Check the spelling, the directory the game starts from, and that the line has no `#`. The Trace log shows the chosen renderer, threads and display mode. |
| Anything else | Run the Debug variant and keep `GLIDULAT.LOG`. Report it at <https://github.com/ElTentakel/Glidulator/issues> with the log and the variant used. |
