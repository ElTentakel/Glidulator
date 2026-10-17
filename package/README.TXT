=========================================================================
  ElTentakel Graphics Systems  *  GLIDULATOR GL-1  *  Version @VERSION@
  The Voodoo-compatible 3D accelerator that fits in a single file.
=========================================================================

This is a PREVIEW release. It runs the tested games and demos (COMPAT.TXT)
but still has known issues (see below). Reports are welcome.

WHAT IT IS
  A software implementation of the 3dfx Voodoo Graphics accelerator and
  its Glide 2.x programming interface for MS-DOS and Windows 95/98/ME.
  Drop one file next to a late-90s Glide game, and it renders through
  Glidulator instead of a Voodoo card. Under plain DOS it puts the other
  CPU cores to work as additional render units.

  ElTentakel Graphics Systems is a fictional company. 3dfx, Voodoo and
  Glide are trademarks of their respective owners; Glidulator is not
  affiliated with them.

CONTENTS
  BIN\GLIDE2X.OVL        Glide for DOS games (release variant)
  BIN\GLIDE2X.DLL        Glide for Windows 9x games (release variant)
  BIN\TRACE\             Trace variant: writes GLIDULAT.LOG (performance)
  BIN\DEBUG\             Debug variant: detailed log, crash handling
  GLIDULAT.CFG           Example settings, every setting with its default
  MANUAL.TXT             User Guide: installation, all settings, help
  COMPAT.TXT             Tested games and demos, known issues
  COPYING.TXT            GNU General Public License, version 2

INSTALLATION
  DOS:      copy BIN\GLIDE2X.OVL into the game directory (replacing the
            game's own file) and start the game with its 3Dfx option.
            Best: boot DOS without memory manager or with HIMEM only;
            then Glidulator uses all CPU cores and its JIT compiler.
  Windows:  copy BIN\GLIDE2X.DLL into the game directory (replacing the
            game's own file) and start the game with its 3Dfx option.
            Keep a game's 3dfx MiniGL (OPENGL32.DLL); it calls Glide.
  Settings: optional. Copy GLIDULAT.CFG next to the game and remove the
            # in front of what you want to change. GLIDULATOR_OSD=1 shows
            frame rate and render path on screen.

REQUIREMENTS
  386 (Windows: 486) or better; for good speed a CPU with SSE2 (JIT) and,
  under DOS, several cores. DOS: VESA VBE 2.0 with linear framebuffer for
  the best picture, otherwise VGA 320x200. DOS games bring their own
  DOS/4GW.

NEW IN 0.9.1
  - Windows 98: GLQuake no longer freezes when its first demo starts.
  - GLIDULATOR_BOARD=rush reports a Voodoo Rush, for Rush patches such as
    Tomb Raider's TUMBRUSH.EXE.
  - DOS: Print Screen saves the current frame as SHOT0000.BMP, ...
  - DOS: multi-core rendering also after Windows 98's "Restart in MS-DOS
    mode" (tested in a virtual machine so far).
  - DOS: fixes a hang ("internal stack overflow") after a multi-core run
    on some machines.
  - The on-screen display shows the cores in use and available.

KNOWN ISSUES
  - DOS: some machines with a lot of RAM refuse DOS/4GW games with
    "DOS/16M error: [40]" when a memory manager is loaded. Boot without.
  - SBEMU (sound for DOS on modern machines): works with the DPMI host
    HDPMI32i (several cores, JIT). The VDPMI pre-release is experimental
    (Dimension hangs).
  - Rasterizer: single pixels can stay open on shared triangle edges.
  - Glide functions without effect: grAA* (anti-aliased primitives),
    guMP*, guTexCreateColorMipMap. No tested title uses them.
  - Windows 95/98/ME render on one CPU core (limit of the system).

REPORTING PROBLEMS
  Use the Debug variant, reproduce the problem and attach GLIDULAT.LOG
  from the game directory, together with the game, the system (DOS or
  Windows, memory manager, CPU, graphics card) and what happened:
  https://github.com/ElTentakel/Glidulator/issues

LICENSE AND SOURCE
  Glidulator is free software under the GNU General Public License,
  version 2 or later (COPYING.TXT). Source code, documentation and
  credits (PCem/86Box rasterizer, MGA-Glide ABI description):
  https://github.com/ElTentakel/Glidulator

  No tentacles were harmed in the making of this software.
