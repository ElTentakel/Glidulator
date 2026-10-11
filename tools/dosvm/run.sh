#!/bin/sh
# Runs Nature (and optionally test programs) in QEMU on FreeDOS 1.3 with a
# selectable memory driver or DPMI host (HIMEM.SYS 3.95 of Windows 98,
# HIMEMX, EMM386, none, SBEMU with HDPMI32i or VDPMI), CPU count/topology
# and OVL; for multi-core rendering and the DPMI paths without hardware.
# Screenshot after <n> seconds.
#
#   tools/dosvm/run.sh <RAM in MB> <himem|himemx|emm386|none|sbemu|vdpmi> [seconds]
#
# sbemu: SBEMU 1.0.0-beta.6 setup (JEMMEX X2MAX=8192 NOEMS, JLOAD QPIEMU.DLL,
#   HDPMI32i -r -x, SBEMU) with an emulated AC97 card; vdpmi: the SBEMU
#   VDPMI pre-release (HIMEMX, VDPMI.EXE, its SBEMU build). Games then run
#   under that DPMI host (ring 3). Packages unzipped into
#   build/dosvm/dl/sbemu and build/dosvm/dl/vdpmi
#   (github.com/crazii/SBEMU/releases). SBEMUARGS="/T6 /I5 ..." are passed
#   to SBEMU.
# none: without memory driver (DOS/4GW without paging, OVL in ring 0).
# SMP=<n>: n CPU cores (QEMU TCG with several threads), for smp_dos.c; also
#   SMP=8,cores=4,threads=2 for a Hyper-Threading topology.
# OVL=<file>: this OVL instead of build/ovl/GLIDE2X.OVL (e.g. trace/debug);
#   the demo's GLIDULAT.LOG is then stored next to the picture (<...>.log).
# NATUREONLY=<n>: start Nature n times (default 1, 0 = not at all);
#   OVLTEST=<n>: run OVLTEST.EXE n times first (make dosbox-test), output
#   to <...>.ovltest.
# CFG="NAME=VALUE;..." writes GLIDULAT.CFG next to Nature.
# KEYS="w8 2 w1 n ...": keys (QEMU sendkey) with pauses wN (seconds) from
#   QEMU's start; shot:NAME = screenshot to build/dosvm/run/NAME.png;
#   mon:COMMAND = QEMU monitor command ('_' = blank, e.g. mon:mce_-b_0_1_...).
# NATUREKEYS=1 = configuration 3DFX, 320x240, no sound.
# R3PROBE="[AP]": run R3PROBE.EXE (r3probe.c: CR3/page walk/APIC under the
#   DPMI host, with AP also starting the other cores) before Nature;
#   result in build/dosvm/run/<...>.r3probe. Use with NATUREONLY=0.
#
# QEMU_MACHINE="pc,max-ram-below-4g=3500M" mimics real boards (more memory
# below 4 GB than QEMU's default of 3 GB).
# QEMU_OPTS="-enable-kvm -cpu host" uses the real CPU (KVM).
# DOSHIGH=1 loads DOS into the HMA (like Windows 98).
# EMMARGS="..." replaces the EMM386 parameters (default NOEMS).
#
# Result: build/dosvm/run/<RAM>_<driver>.png, the demo's log next to it and
# its multi-core/renderer lines on stdout.
# Requirements: build/dosvm/dl/fd/FD13LITE.img (FreeDOS 1.3 LiteUSB,
# ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/),
# build/dosvm/win98/HIMEM.SYS/EMM386.EXE (from the win98qi archive),
# build/compat/nature/ (run_nature.sh), Open Watcom in /opt/watcom.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RAM=$1
XMS=$2
WAIT=${3:-25}
V=$ROOT/build/dosvm
RUN=$V/run
mkdir -p "$RUN" "$V/src"
# R3PROBE.EXE (r3probe.c, DOS/4GW) with the OVL's AP trampoline
W=/opt/watcom
OW="env WATCOM=$W INCLUDE=$W/h PATH=$W/binl64:$PATH"
T=$ROOT/tools/dosvm
make -s -C "$ROOT" build/ovl/ap_tramp.h
if [ ! -f "$V/src/R3PROBE.EXE" ] || [ "$T/r3probe.c" -nt "$V/src/R3PROBE.EXE" ] || [ "$ROOT/build/ovl/ap_tramp.h" -nt "$V/src/R3PROBE.EXE" ]; then
    $OW wcc386 -bt=dos -mf -3r -zq -zastd=c99 -i="$ROOT/build/ovl" -fo="$V/src/r3probe.obj" "$T/r3probe.c"
    $OW wlink system dos4g option quiet name "$V/src/R3PROBE.EXE" file "$V/src/r3probe.obj"
fi
IMG=$RUN/vm.img
cp "$V/dl/fd/FD13LITE.img" "$IMG"
RC=$RUN/mtoolsrc
echo "drive c: file=\"$IMG\" offset=$((63 * 512))" > "$RC"
export MTOOLSRC=$RC MTOOLS_NO_VFAT=1

case $XMS in
himem)  DEV='DEVICE=\HIMEM.SYS' ;;
himemx) DEV='DEVICE=\FREEDOS\BIN\HIMEMX.EXE' ;;
none)   DEV='REM no memory driver' ;;
sbemu)  DEV='DEVICE=\SBEMU\JEMMEX.EXE X2MAX=8192 NOEMS' ;;
vdpmi)  DEV='DEVICE=\SBEMU\HIMEMX.EXE
DEVICE=\SBEMU\VDPMI.EXE' ;;
emm386) DEV="DEVICE=\\HIMEM.SYS
DEVICE=\\EMM386.EXE ${EMMARGS:-NOEMS}" ;;
*) echo "driver: himem|himemx|emm386|none|sbemu|vdpmi" >&2; exit 1 ;;
esac
printf '%s\r\n%sFILES=40\r\nBUFFERS=20\r\nSHELL=\\COMMAND.COM \\ /E:1024 /P=\\FDAUTO.BAT\r\n' "$DEV" "${DOSHIGH:+DOS=HIGH
}" | sed 's/\r*$/\r/' > "$RUN/FDCONFIG.SYS"
{
    printf '@echo off\r\nSET PATH=\\FREEDOS\\BIN\r\n'
    case $XMS in
    sbemu) printf '\\SBEMU\\JLOAD \\SBEMU\\QPIEMU.DLL\r\n\\SBEMU\\HDPMI32I -r -x\r\n\\SBEMU\\SBEMU %s\r\n' "${SBEMUARGS:-}" ;;
    vdpmi) printf '\\SBEMU\\SBEMU %s\r\n' "${SBEMUARGS:-}" ;;
    esac
    [ -n "${R3PROBE+x}" ] && printf 'cd \\DIAG\r\nR3PROBE %s\r\ncd \\\r\n' "$R3PROBE"
    printf 'cd \\NATURE\r\n'
    for i in $(seq "${OVLTEST:-0}"); do printf 'OVLTEST 2\r\ntype OVLTEST.LOG >> ALL.LOG\r\n'; done
    for i in $(seq "${NATUREONLY:-1}"); do printf 'NATURE\r\n'; done
    printf 'echo END\r\n'
} > "$RUN/FDAUTO.BAT"
mdel c:/SETUP.BAT 2>/dev/null || true
mcopy -o "$RUN/FDCONFIG.SYS" "$RUN/FDAUTO.BAT" "$V/win98/HIMEM.SYS" "$V/win98/EMM386.EXE" c:/
mmd c:/DIAG
case $XMS in
sbemu|vdpmi)
    mmd c:/SBEMU
    for f in "$V/dl/$XMS"/*; do
        case $f in *.zip | *.md | *.txt | *.TXT | *CONFIG.SYS) continue ;; esac
        mcopy "$f" "c:/SBEMU/$(basename "$f" | tr a-z A-Z)"
    done ;;
esac
mcopy "$V/src/R3PROBE.EXE" c:/DIAG/
mcopy /opt/watcom/binw/dos4gw.exe c:/DIAG/DOS4GW.EXE
mmd c:/NATURE
N=$ROOT/build/compat/nature
OVLFILE=${OVL:-$ROOT/build/ovl/GLIDE2X.OVL}
mcopy "$N/NATURE.EXE" "$N/NATURE.DAT" "$N/LV-NATUR.S3M" c:/NATURE/
mcopy "$OVLFILE" c:/NATURE/GLIDE2X.OVL
[ -n "${OVLTEST:-}" ] && mcopy "$ROOT/build/dosbox/OVLTEST.EXE" c:/NATURE/
# No frame capture from the debug OVL: 16 x 2 MB would fill the 32 MB
# disk image, after which GLIDULAT.LOG could not grow any more
printf '0 1 0\r\n' > "$RUN/CAPTURE.CFG"
mcopy "$RUN/CAPTURE.CFG" c:/NATURE/
if [ -n "${CFG:-}" ]; then
    echo "$CFG" | tr ';' '\n' | sed 's/$/\r/' > "$RUN/GLIDULAT.CFG"
    mcopy "$RUN/GLIDULAT.CFG" c:/NATURE/
fi
mcopy /opt/watcom/binw/dos4gw.exe c:/NATURE/DOS4GW.EXE

SOCK=$RUN/mon.sock
rm -f "$SOCK"
SMPOPT=
SOUND=
# WAV=1: the card's output goes to build/dosvm/run/sound.wav
case $XMS in sbemu|vdpmi)
    AUD=none
    [ -n "${WAV:-}" ] && AUD="wav,path=$RUN/sound.wav"
    SOUND="-audiodev $AUD,id=snd -device AC97,audiodev=snd" ;;
esac
[ -n "${SMP:-}" ] && SMPOPT="-smp $SMP -accel tcg,thread=multi"
qemu-system-i386 -m "$RAM" ${QEMU_MACHINE:+-machine $QEMU_MACHINE} $SMPOPT $SOUND ${QEMU_OPTS:-} -drive file="$IMG",format=raw -display none \
    -monitor unix:"$SOCK",server,nowait >/dev/null 2>&1 &
Q=$!
[ -n "${NATUREKEYS:-}" ] && KEYS="w10 2 w1 n n n n w1 1 w1 1 w1 y"
T0=$(date +%s)
if [ -n "${KEYS:-}" ]; then
    python3 -c "
import socket,sys,time
time.sleep(1)
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
for k in sys.argv[2].split():
    if k[0]=='w' and len(k)>1: time.sleep(float(k[1:])); continue
    if k.startswith('mon:'): s.send((k[4:].replace('_', ' ') + '\n').encode()); time.sleep(1); continue
    if k.startswith('shot:'): s.send(('screendump %s/%s.ppm\n' % (sys.argv[3], k[5:])).encode()); time.sleep(1); continue
    s.send(('sendkey %s\n' % k).encode()); time.sleep(0.3)
" "$SOCK" "$KEYS" "$RUN"
fi
REST=$((WAIT - ($(date +%s) - T0)))
[ "$REST" -gt 0 ] && sleep "$REST"
OUT=$RUN/${RAM}_${XMS}${SMP:+_smp$SMP}
printf 'screendump %s.ppm\nquit\n' "$OUT" | python3 -c "
import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
for l in sys.stdin: s.send(l.encode()); time.sleep(1)
" "$SOCK"
wait $Q 2>/dev/null || true
python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]+'.ppm').save(sys.argv[1]+'.png')" "$OUT"
rm -f "$OUT.ppm"
for f in "$RUN"/*.ppm; do
    [ -f "$f" ] && python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[1][:-4]+'.png')" "$f" && rm -f "$f"
done
mtype c:/NATURE/GLIDULAT.LOG 2>/dev/null | tr -d '\r' > "$OUT.log" || true
[ -n "${R3PROBE+x}" ] && { mtype c:/DIAG/R3PROBE.TXT 2>/dev/null | tr -d '\r' > "$OUT.r3probe" || true; sed 's/^/  /' "$OUT.r3probe"; }
[ -n "${OVLTEST:-}" ] && { mtype c:/NATURE/ALL.LOG 2>/dev/null | tr -d '\r' > "$OUT.ovltest" || true; }
grep -E "Multi-core|Renderer|worker" "$OUT.log" 2>/dev/null | head -20 | sed 's/^/  /' || true
