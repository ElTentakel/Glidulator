#!/bin/sh
# Windows counterpart of run_nature.sh: NATURE95.EXE (Vertigo 1997) with
# Glidulator's glide2x.dll (make win98) under Wine on Xvfb. The bundled 3dfx
# GLIDE2X.DLL is removed so ours is certainly loaded.
#
#   tools/compat/run_nature95.sh [count] [interval]
#
# Start dialog via xdotool: "No Sound", driver "3DFX", 640x480, OK. Then
# <count> screenshots (default 4) every <interval> seconds (default 15) to
# build/compat/nature95/run/frame_*.png (Xvfb 640x480, i.e. unscaled).
#
# SCREEN=1280x720 selects another Xvfb size (to test output settings, e.g.
# with GLIDULATOR_DISPLAY=desktop GLIDULATOR_OSD=1; Wine passes the
# GLIDULATOR_* variables to the DLL).
#
# DLL=build/win98_trace/glide2x.dll uses another DLL (GLIDULAT.LOG).
#
# Needs Xvfb, xdotool and classic 32-bit Wine (Ubuntu: wine32:i386; the
# "experimental WoW64 mode" of 64-bit Wine crashes at the demo's DirectDraw
# start, also with 3dfx's original DLL).
set -eu
# The start-up logo is on by default; keep it out of screenshots and
# timings unless asked for (GLIDULATOR_SPLASH=gl1 ...).
export GLIDULATOR_SPLASH="${GLIDULATOR_SPLASH:-off}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-4}
GAP=${2:-15}
ZIP=$ROOT/build/compat/nature/vgonatur.zip
URL=https://files.scene.org/get/mirrors/hornet/demos/1997/v/vgonatur.zip
DIR=$ROOT/build/compat/nature95
RUN=$DIR/run
W=/usr/lib/i386-linux-gnu/wine
[ -x "$W/wine" ] || { echo "32-bit Wine missing ($W/wine, package wine32:i386)" >&2; exit 1; }

make -C "$ROOT" win98 >/dev/null
mkdir -p "$(dirname "$ZIP")"
[ -f "$ZIP" ] || curl -sSL -o "$ZIP" "$URL"
rm -rf "$RUN"
mkdir -p "$RUN"
(cd "$RUN" && unzip -q "$ZIP" NATURE95.EXE MIDAS11.DLL NATURE.DAT LV-NATUR.S3M)
cp "${DLL:-$ROOT/build/win98/glide2x.dll}" "$RUN/glide2x.dll"

export WINEPREFIX=$DIR/prefix WINEARCH=win32 WINELOADER=$W/wine WINESERVER=$W/wineserver WINEDEBUG=-all
DISP=95
while [ -e /tmp/.X11-unix/X$DISP ] || [ -e /tmp/.X$DISP-lock ]; do DISP=$((DISP + 1)); done
export DISPLAY=:$DISP
Xvfb "$DISPLAY" -screen 0 "${SCREEN:-640x480}x24" >/dev/null 2>&1 &
XVFB=$!
cleanup() {
    "$W/wineserver" -k 2>/dev/null || true
    kill "$XVFB" 2>/dev/null || true
}
trap cleanup EXIT
sleep 2
[ -d "$WINEPREFIX" ] || { "$W/wine" wineboot -i >/dev/null 2>&1; "$W/wineserver" -w; }

(cd "$RUN" && "$W/wine" NATURE95.EXE >"$RUN/wine.log" 2>&1 &)
WIN=
for _i in $(seq 30); do
    WIN=$(xdotool search --name "NATURE" 2>/dev/null | head -n 1) && [ -n "$WIN" ] && break
    sleep 1
done
[ -n "$WIN" ] || { echo "start dialog does not appear (see $RUN/wine.log)" >&2; exit 1; }
sleep 2
# coordinates relative to the 600x350 dialog
for xy in "201 50" "201 240" "481 240" "41 85"; do
    # shellcheck disable=SC2086
    xdotool mousemove --window "$WIN" $xy click 1
    sleep 0.5
done
for i in $(seq "$N"); do
    sleep "$GAP"
    xwd -root -silent >"$RUN/frame.xwd"
    python3 -c "import sys; sys.path.insert(0, '$ROOT/tools/dosbox_test'); import xwd; xwd.to_png('$RUN/frame.xwd', '$RUN/frame_$i.png')"
    echo "frame_$i.png"
done
rm -f "$RUN/frame.xwd"
