#!/bin/sh
# Compatibility test with further real DOS Glide demos (pattern:
# run_nature.sh). The demos are NOT kept in the repository but downloaded to
# build/compat/<name>/. A bundled GLIDE2X.OVL is replaced by the debug OVL
# (make ovl-debug); a bundled DOS4GW.EXE stays (the path the demo takes on
# real hardware), otherwise Open Watcom's is used.
#
#   tools/compat/run_demo.sh <name> [session]
#
# name: see tools/compat/demos.sh (demos and game demos)
# session: key sequence for tools/dosbox_test/session.py, default
#   tools/compat/<name>.session (if present) or a few screenshots.
# Result: build/compat/<name>/run/*.png and GLIDULAT.LOG.
# Environment: MEMSIZE=<MB> (DOSBox memory), CAPTURE="first interval count"
# (frame capture; replay: build/linux/glide_replay <dir> CAPnn.BIN ...),
# GLCFG="NAME=value ..." (written as GLIDULAT.CFG next to every OVL).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME=${1:?demo name missing}
. "$ROOT/tools/compat/demos.sh"
demo_info "$NAME"
SESSION=${2:-$ROOT/tools/compat/$NAME.session}

make -C "$ROOT" ovl-debug >/dev/null
RUN=$ROOT/build/compat/$NAME/run
demo_unpack "$NAME" "$RUN"
demo_install_ovl "$ROOT/build/ovl_debug/GLIDE2X.OVL" "$RUN"
# CAPTURE="first interval count": which frames the debug OVL records as
# CAPnn.BIN (default 5 5 16, see core/capture.c).
if [ -n "${CAPTURE:-}" ]; then
    echo "$CAPTURE" > "$RUN/CAPTURE.CFG"
    demo_spread_file "$RUN/CAPTURE.CFG" "$RUN"
fi
if [ -n "${GLCFG:-}" ]; then
    printf '%s\n' $GLCFG > "$RUN/GLIDULAT.CFG"
    demo_spread_file "$RUN/GLIDULAT.CFG" "$RUN"
fi
# @CMD@ in the template -> "cd <start directory>" + start command. Values go
# to awk through the environment (awk -v would interpret the backslashes of
# the DOS path).
DOSDIR=$(echo "$CDIR" | tr '/' '\\') CMD="$CMD" MEM="${MEMSIZE:-63}" awk '
    /^@CMD@$/   { print "cd " ENVIRON["DOSDIR"]; print ENVIRON["CMD"]; next }
    /^memsize=/ { print "memsize=" ENVIRON["MEM"]; next }
    { print }' "$ROOT/tools/compat/demo.dosbox.conf" > "$RUN/dosbox.conf"

if [ ! -f "$SESSION" ]; then
    SESSION=$RUN/default.session
    printf 'wait 2\nshots frame 20 3\n' > "$SESSION"
fi
python3 "$ROOT/tools/dosbox_test/session.py" "$RUN" "$SESSION"
echo "--- first calls (GLIDULAT.LOG) ---"
# Some demos change into their data directory first (Fucktory: DATA\).
LOG=$(find "$RUN" -name GLIDULAT.LOG | head -n 1)
if [ -n "$LOG" ]; then
    echo "($LOG)"
    grep "first call\\|failed" "$LOG" || true
else
    echo "(no GLIDULAT.LOG -- OVL never loaded?)"
fi
