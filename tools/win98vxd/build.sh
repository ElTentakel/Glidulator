#!/bin/sh
# Builds the Windows 9x test programs for GLSMP.VXD (see README.md):
#   VXDLOAD.EXE  drives the VxD directly (start/stop the cores, run jobs)
#   GLMTTEST.EXE renders through glide2x.dll, multi-core against one core
# The VxD itself comes from the Makefile (make vxd -> build/win98vxd/).
# GLMTTEST links against glide2x.dll, which exports "_name@n"
# (targets/glide_dll/glide2x.def); the import library maps the plain stdcall
# names onto them.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MINGW=${MINGW:-i686-w64-mingw32-gcc}
DLLTOOL=${DLLTOOL:-i686-w64-mingw32-dlltool}
OUT=$ROOT/build/win98vxd

make -C "$ROOT" -s vxd
"$MINGW" -O2 -mwindows -o "$OUT/VXDLOAD.EXE" "$HERE/vxdload.c"
{
    echo "LIBRARY glide2x.dll"
    echo "EXPORTS"
    awk '/^ +_[A-Za-z0-9_]+@[0-9]+ = /{print "    " $3 " == " $1}' "$ROOT/targets/glide_dll/glide2x.def"
} > "$OUT/glide2x_imp.def"
"$DLLTOOL" -d "$OUT/glide2x_imp.def" -l "$OUT/libglide2x.a"
"$MINGW" -O2 -Wall -mwindows -I"$ROOT/core/include" -o "$OUT/GLMTTEST.EXE" "$HERE/glmttest.c" "$OUT/libglide2x.a"
echo "built in build/win98vxd: GLSMP.VXD, VXDLOAD.EXE, GLMTTEST.EXE"
