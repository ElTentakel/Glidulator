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
  - The 3dfx Glide 2.x source release (sezero/glide), the reference for
    driver behaviour.

CONTENTS
  BIN\GLIDE2X.OVL        Glide for DOS games (release variant)
  BIN\GLIDE2X.DLL        Glide for Windows 9x games (release variant)
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

NEW IN 0.9.3
  - Mipmap dithering as on the Voodoo board (GLIDULATOR_LOD_DITHER=
    auto|1|0): no hard bands between mipmap levels.
  - The OSD shows how the other cores run (SMP RING0, PAGING, FLAT,
    GLSMP, THREADS or OFF). GLIDULATOR_SCREENSHOT=2 includes the OSD.
  - Hyper-Threading with the BIOS option "CPUID Limit" is recognised
    (DOS and Windows 9x); siblings no longer count as cores.
  - 8.4 MB less memory for the texture cache: Pyl loads its levels
    without HDPMI32i.
  - Shared triangle edges are drawn exactly once (start-up logo).

KNOWN ISSUES
  - DOS: some machines with a lot of RAM refuse DOS/4GW games with
    "DOS/16M error: [40]" when a memory manager is loaded. Boot without.
  - SBEMU (sound for DOS on modern machines): works with the DPMI host
    HDPMI32i (several cores, JIT). The VDPMI pre-release is experimental
    (Dimension hangs).
  - Glide functions without effect: grAA* (anti-aliased primitives),
    guMP*, guTexCreateColorMipMap. No tested title uses them.
  - Windows 95/98/ME: multi-core rendering (GLSMP.VXD) is new, tested in
    a virtual machine and on one real PC. If a game misbehaves with it,
    set GLIDULATOR_SMP=off or remove GLSMP.VXD.
  - Hyper-Threading with the BIOS option "CPUID Limit": the detection is
    new and tested in a virtual machine only. Please report if the OSD
    shows more cores than the CPU has.

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
