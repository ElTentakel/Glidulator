#!/bin/sh
# Starts the Glide demo "Nature" (Vertigo, 1997) VISIBLY in DOSBox with
# Glidulator's GLIDE2X.OVL instead of a real Voodoo board, to watch it.
# (Automated headless run with screenshots: run_nature.sh.)
#
#   tools/compat/play_nature.sh            window 1280x800
#   tools/compat/play_nature.sh -f         full screen
#   tools/compat/play_nature.sh -t         with trace OVL (GLIDULAT.LOG)
#   tools/compat/play_nature.sh -s         without sound
#   tools/compat/play_nature.sh -g 1.0     gamma (default 1.3, like SST_GAMMA)
#
# Needs: dosbox, curl, unzip, Open Watcom in /opt/watcom (for the OVL and
# DOS4GW.EXE). The demo is downloaded once to build/compat/nature/ (not in
# the repository).
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR=$ROOT/build/compat/nature
URL=https://files.scene.org/get/mirrors/hornet/demos/1997/v/vgonatur.zip

FULLSCREEN=false
TRACE=0
SOUND=1
GAMMA=
while getopts ftsg: opt; do
    case $opt in
    f) FULLSCREEN=true ;;
    t) TRACE=1 ;;
    s) SOUND=0 ;;
    g) GAMMA=$OPTARG ;;
    *) sed -n '2,14p' "$0"; exit 1 ;;
    esac
done

if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    echo "No display found (DISPLAY/WAYLAND_DISPLAY empty) -- start from a desktop terminal." >&2
    exit 1
fi
for tool in dosbox curl unzip; do
    command -v $tool >/dev/null || { echo "$tool missing (e.g. sudo apt-get install $tool)" >&2; exit 1; }
done

if [ $TRACE = 1 ]; then
    make -C "$ROOT" ovl-trace >/dev/null
    OVL=$ROOT/build/ovl_trace/GLIDE2X.OVL
else
    make -C "$ROOT" ovl >/dev/null
    OVL=$ROOT/build/ovl/GLIDE2X.OVL
fi

mkdir -p "$DIR"
if [ ! -f "$DIR/vgonatur.zip" ]; then
    echo "Downloading the demo from scene.org ..."
    curl -sSL -o "$DIR/vgonatur.zip" "$URL"
fi

PLAY=$DIR/play
mkdir -p "$PLAY"
(cd "$PLAY" && unzip -o -q ../vgonatur.zip NATURE.EXE NATURE.DAT LV-NATUR.S3M NATURE.TXT)
cp "$OVL" "$PLAY/GLIDE2X.OVL"
rm -f "$PLAY/GLIDULAT.CFG"
if [ -n "$GAMMA" ]; then
    # Like the SST_GAMMA environment variable of 3dfx's driver; the OVL
    # reads it from GLIDULAT.CFG in the current directory (demos that change
    # into a subdirectory first need it there as well).
    printf 'SST_GAMMA=%s\r\n' "$GAMMA" > "$PLAY/GLIDULAT.CFG"
    for d in "$PLAY"/*/; do if [ -d "$d" ]; then cp "$PLAY/GLIDULAT.CFG" "$d"; fi; done
fi
cp /opt/watcom/binw/dos4gw.exe "$PLAY/DOS4GW.EXE"
rm -f "$PLAY/GLIDULAT.LOG"

if [ $SOUND = 1 ]; then
    SBTYPE=sb16
else
    SBTYPE=none
fi

# output=opengl + windowresolution: scales every mode to the window
# (output=surface with scaler=normal2x is not always applied by DOSBox 0.74,
# giving a tiny 640x400 window). DOSBox itself falls back to surface without
# OpenGL.
# IRQ 5: according to NATURE.TXT the sound card IRQ must be below 7
# (DOSBox's default is 7). memsize 63: the OVL takes ~22 MB at grSstWinOpen.
cat >"$PLAY/dosbox.conf" <<EOF
[sdl]
fullscreen=$FULLSCREEN
fullresolution=desktop
windowresolution=1280x800
output=opengl
[dosbox]
machine=svga_s3
memsize=63
[render]
scaler=normal2x
aspect=false
[cpu]
core=dynamic
cycles=max
[sblaster]
sbtype=$SBTYPE
irq=5
[gus]
gus=false
[autoexec]
mount c .
c:
NATURE
exit
EOF

cat <<'EOF'
=====================================================================
 Nature (Vertigo 1997) with Glidulator -- how to answer the menu:

   [2]  Configure settings
   n    stereoscopic mode?
   n    high gamma?
   n    widescreen mode?
   n    demo loop?
   1    blit driver: 1 = 3DFX  (= our GLIDE2X.OVL; 0 would be VESA software)
   1    resolution (ignored in 3DFX mode, the demo uses 512x384)
   n/y  disable sound?

 Then ~40 s loading bar, then the textured 3D scenes (in DOSBox only
 ~0.4 fps; the demo is time-based and jumps from frame to frame).

 QUIT:     Ctrl+F9 (quits DOSBox). The "Q" mentioned in NATURE.TXT does
           not react under DOSBox+Glidulator yet (open).
 DOSBox:   Alt+Enter = full screen on/off   Ctrl+F10 = release mouse
=====================================================================
EOF

cd "$PLAY"
dosbox -conf dosbox.conf -noconsole >/dev/null 2>&1 || true

if [ $TRACE = 1 ] && [ -f GLIDULAT.LOG ]; then
    echo "Call log: $PLAY/GLIDULAT.LOG"
fi
