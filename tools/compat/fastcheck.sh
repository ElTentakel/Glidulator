#!/bin/sh
# Fast renderer (GLIDULATOR_RENDERER=fast) against the exact C path over all
# recordings in build/compat (CAP*.BIN of the debug variants): deviation
# per frame (pixels, largest step, PSNR) and the same picture with 1, one
# worker and all threads. Needs make replay32 (the JIT is 32-bit only).
#
#   tools/compat/fastcheck.sh [--lod-dither] [dir ...]
#
# Without dirs every directory under build/compat with CAP*.BIN. Prints
# one line per recording, a summary at the end; exit code 1 if a thread
# count gave a different picture.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
R=$ROOT/build/linux32/glide_replay
[ -x "$R" ] || { echo "make replay32 first" >&2; exit 2; }
OPT=
if [ "${1:-}" = "--lod-dither" ]; then
    OPT=--lod-dither
    shift
fi
if [ $# -eq 0 ]; then
    set -- $(find "$ROOT/build/compat" -name 'CAP*.BIN' -printf '%h\n' | sort -u)
fi
OUT=$(mktemp -d)
bad=0
n=0
for d in "$@"; do
    for f in "$d"/CAP*.BIN; do
        [ -f "$f" ] || continue
        c=$(basename "$f")
        # GLIDULATOR_TMUS=2: recordings with two TMUs need the second one
        res=$(GLIDULATOR_SPLASH=off GLIDULATOR_TMUS=2 "$R" "$d" "$c" "$OUT/x.png" --compare-fast $OPT 2>&1)
        stat=$(printf '%s\n' "$res" | sed -n 's/^compare fast (worker) against c: //p')
        if printf '%s\n' "$res" | grep -q "differs from one worker"; then
            thr="THREADS DIFFER"
            bad=1
        else
            thr="threads ok"
        fi
        n=$((n + 1))
        printf '%s/%s: %s; %s\n' "${d#"$ROOT"/build/compat/}" "$c" "${stat:-no result}" "$thr"
    done
done
rm -rf "$OUT"
echo "$n recordings"
exit $bad
