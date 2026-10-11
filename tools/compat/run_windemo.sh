#!/bin/sh
# Run a Windows Glide program with Glidulator's glide2x.dll (make win98)
# under 32-bit Wine on Xvfb and take screenshots; general form of
# run_nature95.sh (see there for Wine/Xvfb background).
#
#   tools/compat/run_windemo.sh <directory> <exe> [count] [interval] [keys...]
#
# directory: unpacked program (copied to <directory>.run, every bundled
#   glide2x.dll there replaced by ours)
# exe: path relative to directory (started in its folder)
# keys: xdotool key names, sent one after another after the start;
#   click:X:Y clicks at screen position X,Y (Xvfb 640x480), sleep:N waits,
#   rel:DX:DY moves the mouse relatively (DirectInput mouse), press clicks
# Wine runs with a virtual desktop (registry, Explorer\\Desktops): Wine then
# handles window order and foreground by Win32 rules without an X window
# manager; otherwise dialogs ignore Return and DirectInput ignores the
# keyboard.
# Environment: ARGS="..." command line for the program (e.g. /s),
#   WAIT=<s> delay before the keys (default 3), WINVER=<ver> (win98), TRACE=1
#   trace DLL (make win98-trace, log in GLIDULAT.LOG next to the EXE),
#   DEBUG=1 debug DLL (make win98-debug), DLL=<file> a given glide2x.dll
#   (e.g. from build/dist/); WINEDEBUG is passed through
#   (default -all, e.g. WINEDEBUG=+relay)
# Result: <directory>.run/frame_<n>.png, wine.log
set -eu
# The start-up logo is on by default; keep it out of screenshots and
# timings unless asked for (GLIDULATOR_SPLASH=gl1 ...).
export GLIDULATOR_SPLASH="${GLIDULATOR_SPLASH:-off}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$(cd "${1:?directory missing}" && pwd)
EXE=${2:?EXE missing}
N=${3:-3}
GAP=${4:-10}
shift 2; [ $# -gt 0 ] && shift; [ $# -gt 0 ] && shift
W=/usr/lib/i386-linux-gnu/wine
[ -x "$W/wine" ] || { echo "32-bit Wine missing ($W/wine, package wine32:i386)" >&2; exit 1; }

if [ -n "${DLL:-}" ]; then
    DLL=$(realpath "$DLL")    # a given DLL, e.g. from the release package
elif [ -n "${DEBUG:-}" ]; then
    make -C "$ROOT" win98-debug >/dev/null
    DLL=$ROOT/build/win98_debug/glide2x.dll
elif [ -n "${TRACE:-}" ]; then
    make -C "$ROOT" win98-trace >/dev/null
    DLL=$ROOT/build/win98_trace/glide2x.dll
else
    make -C "$ROOT" win98 >/dev/null
    DLL=$ROOT/build/win98/glide2x.dll
fi
RUN=$SRC.run
rm -rf "$RUN"
cp -r "$SRC" "$RUN"
find "$RUN" -iname glide2x.dll -exec rm -f {} +
cp "$DLL" "$RUN/$(dirname "$EXE")/"

export WINEPREFIX=$ROOT/build/compat/wineprefix WINEARCH=win32 WINELOADER=$W/wine WINESERVER=$W/wineserver WINEDEBUG=${WINEDEBUG:--all}
DISP=95
while [ -e /tmp/.X11-unix/X$DISP ] || [ -e /tmp/.X$DISP-lock ]; do DISP=$((DISP + 1)); done
export DISPLAY=:$DISP
Xvfb "$DISPLAY" -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
cleanup() {
    "$W/wineserver" -k 2>/dev/null || true
    kill "$XVFB" 2>/dev/null || true
}
trap cleanup EXIT
sleep 2
[ -d "$WINEPREFIX" ] || { "$W/wine" wineboot -i >/dev/null 2>&1; "$W/wineserver" -w; }
# Windows version: the target platform is Win9x, and some programs check it
# ("runs on Windows 95 only"). WINVER=win95/win98/winxp/...
"$W/wine" winecfg -v "${WINVER:-win98}" >/dev/null 2>&1
# Virtual desktop 640x480 for all programs of this prefix
"$W/wine" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f >/dev/null 2>&1
"$W/wine" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v Default /d 640x480 /f >/dev/null 2>&1

(cd "$RUN/$(dirname "$EXE")" && "$W/wine" "$(basename "$EXE")" ${ARGS:-} >"$RUN/wine.log" 2>&1 &)
sleep ${WAIT:-3}
for k in "$@"; do
    case "$k" in
    click:*) xdotool mousemove $(echo "${k#click:}" | tr ':' ' '); sleep 0.3
             xdotool mousedown 1; sleep 0.3; xdotool mouseup 1 ;;
    sleep:*) sleep "${k#sleep:}" ;;
    rel:*) xdotool mousemove_relative -- $(echo "${k#rel:}" | tr ':' ' ') ;;
    press) xdotool mousedown 1; sleep 0.3; xdotool mouseup 1 ;;
    # Hold keys 0.3 s: games that only poll the current state through
    # DirectInput miss a short key press.
    *) xdotool keydown "$k"; sleep 0.3; xdotool keyup "$k" ;;
    esac
    sleep 1
done
for i in $(seq "$N"); do
    sleep "$GAP"
    # Our output window on top (under Wine/Xvfb without a window manager
    # the X window of some programs stays above it despite HWND_TOPMOST,
    # e.g. GLQuake in windowed mode)
    for id in $(xdotool search --name '^Glidulator$' 2>/dev/null); do xdotool windowraise "$id" 2>/dev/null || true; done
    sleep 0.5
    xwd -root -silent >"$RUN/frame.xwd"
    python3 -c "import sys; sys.path.insert(0, '$ROOT/tools/dosbox_test'); import xwd; xwd.to_png('$RUN/frame.xwd', '$RUN/frame_$i.png')"
    pgrep -f "$(basename "$EXE")" >/dev/null || { echo "program ended (after frame_$i)"; break; }
done
rm -f "$RUN/frame.xwd"
ls "$RUN"/frame_*.png 2>/dev/null
