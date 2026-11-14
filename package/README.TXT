=========================================================================
  ElTentakel Graphics Systems  *  GLIDULATOR GL-1  *  Version @VERSION@
  The Voodoo-compatible 3D accelerator that fits in a single file.
=========================================================================

This is a PREVIEW release. It runs the tested games and demos (COMPAT.TXT)
but still has known issues (see below). Reports are welcome.

WHAT IT IS
  A software implementation of the 3dfx Voodoo Graphics accelerator and
  its Glide 2.x and 3.x programming interfaces for MS-DOS, Windows
  95/98/ME and Windows NT 3.51/4.0 (NT 3.51: copy an MSVCRT.DLL, e.g.
  from Windows 98 or NT 4.0, next to the game).
  Drop one file next to a late-90s Glide game, and it renders through
  Glidulator instead of a Voodoo card. Under plain DOS and Windows
  95/98/ME it puts the other CPU cores to work as additional render units.

  ElTentakel Graphics Systems is a fictional company. 3dfx, Voodoo and
  Glide are trademarks of their respective owners; Glidulator is not
  affiliated with them.

BUILT ON THE WORK OF OTHERS
  Glidulator would not exist without these projects and their authors:
  - PCem by Sarah Walker, as further developed by the 86Box
    contributors (GPL): the Voodoo rasterizer, texture unit, x86 JIT
    and video filter at the heart of Glidulator.
  - MGA-Glide by ranulphus (MIT): the clean-room Glide 2.x ABI
    description.
  - The 3dfx Glide 2.x and 3.x source release (sezero/glide), the
    reference for driver behaviour and the Glide 3.x interface.

CONTENTS
  BIN\GLIDE2X.OVL        Glide for DOS games (release variant)
  BIN\GLIDE2X.DLL        Glide for Windows 9x and NT games (release variant)
  BIN\GLIDE3X.DLL        Glide 3 for Windows games with a Glide 3 renderer
  BIN\GLIDE.DLL          Glide 2.1.1 for the first Voodoo games (1996)
  BIN\GLSMP.VXD          Windows 9x multi-core helper, next to the DLL
  BIN\TRACE\             Trace variant: writes GLIDULAT.LOG (performance)
  BIN\DEBUG\             Debug variant: detailed log, crash handling
  GLIDULAT.CFG           Example settings, every setting with its default
  MANUAL.TXT             User Guide: installation, all settings, help
  COMPAT.TXT             Tested games and demos, known issues
  CHANGES.TXT            Changes of every version
  COPYING.TXT            GNU General Public License, version 2

INSTALLATION
  DOS:      copy BIN\GLIDE2X.OVL into the game directory (replacing the
            game's own file) and start the game with its 3Dfx option.
            Best: boot DOS without memory manager or with HIMEM only;
            then Glidulator uses all CPU cores and its JIT compiler.
  Windows:  copy BIN\GLIDE2X.DLL into the game directory (replacing the
            game's own file) and start the game with its 3Dfx option.
            Games with a Glide 3 renderer (for example Need for Speed:
            Porsche Unleashed) load BIN\GLIDE3X.DLL instead; put it next
            to the game's renderer DLL the same way. The first Voodoo
            games (Scorched Planet, Pandemonium!) load BIN\GLIDE.DLL.
            Copy BIN\GLSMP.VXD next to it to render on several CPU cores
            (loaded only while the game runs, nothing is installed).
            Keep a game's 3dfx MiniGL (OPENGL32.DLL); it calls Glide.
  Settings: optional. Copy GLIDULAT.CFG next to the game and remove the
            # in front of what you want to change. GLIDULATOR_OSD=1 shows
            frame rate and render path on screen.

REQUIREMENTS
  386 (Windows: 486) or better; for good speed a CPU with SSE2 (JIT) and,
  under DOS, several cores. DOS: VESA VBE 2.0 with linear framebuffer for
  the best picture, otherwise VGA 320x200. DOS games bring their own
  DOS/4GW.

NEW IN 0.9.7
  - Glide 2.1.1 (BIN\GLIDE.DLL) for the first Voodoo games: Scorched
    Planet (84-97 frames per second, Windows 98, Core i7-4790),
    Pandemonium!, CyberGladiators.
  - Windows NT 3.51 and 4.0 supported (NT 3.51: MSVCRT.DLL next to the
    game).
  - Fast renderer (GLIDULATOR_RENDERER=fast, optional, needs SSE2):
    not pixel-exact, Half-Life on a Core 2 Duo E8400 31.8 -> 38.1 frames
    per second.
  - Voodoo video filter with SSE2 under Windows: same picture, faster.

KNOWN ISSUES
  - DOS: some machines with a lot of RAM refuse DOS/4GW games with
    "DOS/16M error: [40]" when a memory manager is loaded. Boot without.
  - SBEMU (sound for DOS on modern machines): works with the DPMI host
    HDPMI32i (several cores, JIT). The VDPMI pre-release is experimental
    (Dimension hangs).
  - Pyl with EMM386 but without HDPMI32i crashes while loading a level on
    one tester's machine. Without EMM386, or with HDPMI32i, it loads.
  - Glide functions without effect: grAA* (anti-aliased primitives),
    guMP*, guTexCreateColorMipMap. No tested title uses them.
  - GLIDE.DLL: guFbReadRegion/guFbWriteRegion do nothing; no tested
    game uses them.
  - Windows NT 3.51/4.0: rendering without write-combining and without
    the JIT, so slower than Windows 98 on the same PC. Reported working
    by a user; checked here in virtual machines only.
  - GLIDE3X.DLL: clip coordinates (grCoordinateSpace with
    GR_CLIP_COORDS) are not supported yet; no tested game uses them.
  - Unreal Tournament with two texture units (the Windows default) draws
    lightmapped walls darker than with one. GLIDULATOR_TMUS=1 gives the
    brighter picture; which one matches a real Voodoo2 is not checked yet.
  - Windows 95/98/ME: multi-core rendering (GLSMP.VXD) is tested in a
    virtual machine and on two real PCs (Intel only). If a game
    misbehaves with it, set GLIDULATOR_SMP=off or remove GLSMP.VXD.
  - Windows 98: a game ended through the task list leaves write-combining
    on until the next reboot. Harmless; quit games normally.
  - GLIDULATOR_HT=1 is experimental. On four cores with Hyper-Threading
    it is now about as fast as the default.
  - More than four render workers are tested in virtual machines only.

REPORTING PROBLEMS
  Use the Debug variant, reproduce the problem and attach GLIDULAT.LOG
  from the game directory, together with the game, the system (DOS or
  Windows, memory manager, CPU, graphics card) and what happened:
  https://github.com/ElTentakel/Glidulator/issues

LICENSE AND SOURCE
  Glidulator is free software under the GNU General Public License,
  version 2 or later (COPYING.TXT). Source code, documentation and
  detailed credits:
  https://github.com/ElTentakel/Glidulator

  No tentacles were harmed in the making of this software.
