#!/bin/sh
# Starts one of the DOS Glide demos VISIBLY in DOSBox with Glidulator's
# GLIDE2X.OVL instead of a real Voodoo board, to watch it.
# (Headless run with screenshots: run_demo.sh; Nature: play_nature.sh.)
#
#   tools/compat/play_demo.sh <name>        window 1280x800
#   tools/compat/play_demo.sh -f <name>     full screen
#   tools/compat/play_demo.sh -t <name>     with trace OVL (GLIDULAT.LOG)
#   tools/compat/play_demo.sh -s <name>     without sound card
#   tools/compat/play_demo.sh -l            list of demos
#   tools/compat/play_demo.sh -g 1.0 <name> gamma (default 1.3, like SST_GAMMA)
#
# name: see -l (demos like dimension, game demos like gta; nature -> play_nature.sh)
# Needs: dosbox, curl, unzip, Open Watcom in /opt/watcom. The demos are
# downloaded once to build/compat/<name>/ (not in the repository; source
# URLs in tools/compat/demos.sh).
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tools/compat/demos.sh"

FULLSCREEN=false
TRACE=0
SOUND=1
GAMMA=
LIST=0
while getopts ftslg: opt; do
    case $opt in
    f) FULLSCREEN=true ;;
    t) TRACE=1 ;;
    s) SOUND=0 ;;
    l) LIST=1 ;;
    g) GAMMA=$OPTARG ;;
    *) sed -n '2,16p' "$0"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

if [ $LIST = 1 ] || [ $# -lt 1 ]; then
    echo "Demos:"
    for d in $DEMOS; do
        demo_info "$d"
        echo "  $d -- $(echo "$MENU" | head -n 1)"
    done
    echo "  nature -- Nature (Vertigo 1997), own script play_nature.sh"
    [ $LIST = 1 ] && exit 0
    echo "Usage: $0 [-f] [-t] [-s] <name>" >&2
    exit 1
fi
NAME=$1
if [ "$NAME" = nature ]; then
    # Nature has its own script (different menu, different loader).
    ARGS=""
    [ $FULLSCREEN = true ] && ARGS="$ARGS -f"
    [ $TRACE = 1 ] && ARGS="$ARGS -t"
    [ $SOUND = 0 ] && ARGS="$ARGS -s"
    [ -n "$GAMMA" ] && ARGS="$ARGS -g $GAMMA"
    exec "$ROOT/tools/compat/play_nature.sh" $ARGS
fi
demo_info "$NAME"

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

PLAY=$ROOT/build/compat/$NAME/play
demo_unpack "$NAME" "$PLAY"
demo_install_ovl "$OVL" "$PLAY"
if [ -n "$GAMMA" ]; then
    # Like the SST_GAMMA environment variable of 3dfx's driver; the OVL
    # reads it from GLIDULAT.CFG in the current directory (programs that
    # change into a subdirectory first need it there as well).
    printf 'SST_GAMMA=%s\r\n' "$GAMMA" > "$PLAY/GLIDULAT.CFG"
    demo_spread_file "$PLAY/GLIDULAT.CFG" "$PLAY"
fi
DOSDIR=$(echo "$CDIR" | tr '/' '\\')

if [ $SOUND = 1 ]; then
    SBTYPE=sb16
else
    SBTYPE=none
fi

# As in play_nature.sh: output=opengl + windowresolution scales every video
# mode to the window. Sound card SB16 at port 220, IRQ 5, DMA 1, HDMA 5
# (what the demos' setup menus usually suggest).
cat >"$PLAY/dosbox.conf" <<CONF
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
sbbase=220
irq=5
dma=1
hdma=5
[gus]
gus=false
[autoexec]
mount c .
c:
cd $DOSDIR
$CMD
exit
CONF

cat <<BANNER
=====================================================================
 $MENU

 Note: in DOSBox the OVL renders only ~0.4 fps (emulated CPU); the
 demos are time-based and therefore jump from frame to frame.

 QUIT:     Escape (usually quits the demo) or Ctrl+F9 (quits DOSBox)
 DOSBox:   Alt+Enter = full screen on/off   Ctrl+F10 = release mouse
=====================================================================
BANNER

cd "$PLAY"
dosbox -conf dosbox.conf -noconsole >/dev/null 2>&1 || true

if [ $TRACE = 1 ]; then
    LOG=$(find "$PLAY" -name GLIDULAT.LOG | head -n 1)
    [ -n "$LOG" ] && echo "Call log: $LOG"
fi
