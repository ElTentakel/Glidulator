#!/bin/sh
# Compatibility test with a real Glide demo: "Nature" by Vertigo (1997, DOS
# + Win95; loads GLIDE2X.OVL via DOS/4GW's LINEXE_LOADMODULE, the same way
# real games do). The demo is NOT kept in the repository (license: pass on
# unchanged and non-commercially only); this script downloads it to
# build/compat/nature/ and runs it with the debug OVL (make ovl-debug) in
# DOSBox, keys via tools/dosbox_test/session.py.
#
# Result: build/compat/nature/run/frame_*.png and GLIDULAT.LOG (which Glide
# functions the demo calls first and when), plus frame captures.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR=$ROOT/build/compat/nature
URL=https://files.scene.org/get/mirrors/hornet/demos/1997/v/vgonatur.zip

make -C "$ROOT" ovl-debug >/dev/null
mkdir -p "$DIR"
[ -f "$DIR/vgonatur.zip" ] || curl -sSL -o "$DIR/vgonatur.zip" "$URL"
(cd "$DIR" && unzip -o -q vgonatur.zip NATURE.EXE NATURE.DAT LV-NATUR.S3M)

RUN=$DIR/run
rm -rf "$RUN"
mkdir -p "$RUN"
cp "$DIR/NATURE.EXE" "$DIR/NATURE.DAT" "$DIR/LV-NATUR.S3M" "$ROOT/build/ovl_debug/GLIDE2X.OVL" "$RUN/"
cp /opt/watcom/binw/dos4gw.exe "$RUN/DOS4GW.EXE"
cp "$ROOT/tools/compat/nature.dosbox.conf" "$RUN/dosbox.conf"

python3 "$ROOT/tools/dosbox_test/session.py" "$RUN" "$ROOT/tools/compat/nature.session"
echo "--- first calls (GLIDULAT.LOG) ---"
grep "first call\\|failed" "$RUN/GLIDULAT.LOG" || true
