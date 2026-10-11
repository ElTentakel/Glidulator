#!/bin/sh
# Runs OVLTEST.EXE (targets/glide_dos_ovltest) with the real GLIDE2X.OVL in
# DOSBox on an Xvfb display and checks by screenshot what was really on the
# screen. Called from the Makefile (make dosbox-test):
#
#   run.sh <stage-directory> <machine> <extender> <expectation>
#
#   machine:     svga_s3 (VESA/VBE 2.0) | vgaonly (no VESA -> mode 13h)
#   extender:    DOS4GW | DOS32A          (which one starts OVLTEST.EXE)
#   expectation: vbe | mode13             (see check_screenshot.py)
#
# Needs dosbox (0.74 tested), Xvfb, xwd.
set -eu

STAGE=$1
MACHINE=$2
EXTENDER=$3
EXPECT=$4
HERE=$(cd "$(dirname "$0")" && pwd)
RUN="$STAGE/run_${MACHINE}_${EXTENDER}"
HOLD=6

rm -rf "$RUN"
mkdir -p "$RUN"
cp "$STAGE/OVLTEST.EXE" "$STAGE/GLIDE2X.OVL" "$STAGE/DOS4GW.EXE" "$STAGE/DOS32A.EXE" "$RUN/"

if [ "$EXTENDER" = DOS4GW ]; then
    CMD="OVLTEST $HOLD"  # the Watcom stub starts DOS4GW.EXE itself
    GAMMA=1.3            # the OVL's default (core/glide_sst.c)
else
    CMD="$EXTENDER OVLTEST.EXE $HOLD"
    # The DOS/32A runs also test GLIDULAT.CFG (replacement for the
    # SST_GAMMA environment variable of 3dfx's driver): linear.
    GAMMA=1.0
    printf 'SST_GAMMA=1.0\r\n' > "$RUN/GLIDULAT.CFG"
fi

cat > "$RUN/dosbox.conf" <<EOF
[sdl]
output=surface
[dosbox]
machine=$MACHINE
memsize=63
[render]
scaler=none
aspect=false
[cpu]
core=auto
cycles=max
[mixer]
nosound=true
[speaker]
pcspeaker=false
[autoexec]
mount c .
c:
$CMD
exit
EOF

# Find a free display, start Xvfb, clean up on exit.
DISP=90
while [ -e /tmp/.X11-unix/X$DISP ] || [ -e /tmp/.X$DISP-lock ]; do DISP=$((DISP + 1)); done
Xvfb :$DISP -screen 0 1024x768x24 >/dev/null 2>&1 &
XPID=$!
trap 'kill $XPID 2>/dev/null || true' EXIT INT TERM
sleep 1

(cd "$RUN" && DISPLAY=:$DISP SDL_AUDIODRIVER=dummy timeout 90 dosbox -conf dosbox.conf -noconsole \
    >dosbox.out 2>&1; echo $? >dosbox.rc) &
DPID=$!

# While OVLTEST keeps the picture for HOLD seconds, take a screenshot
# every 0.5 s; check_screenshot.py picks the matching one.
i=0
while kill -0 $DPID 2>/dev/null; do
    i=$((i + 1))
    n=$(printf %03d $i)
    DISPLAY=:$DISP xwd -root -silent -out "$RUN/shot$n.xwd" 2>/dev/null || true
    # plus the window geometry: the check recognises the mode switch by
    # it (graphics mode -> back to text mode).
    DISPLAY=:$DISP xwininfo -root -tree 2>/dev/null | grep '"DOSBox' >"$RUN/shot$n.geo" || true
    sleep 0.5
done
wait $DPID || true

python3 "$HERE/check_screenshot.py" "$RUN" "$EXPECT" "$GAMMA"
