#!/bin/sh
# GLQuake for Glidulator tests: Quake 1.06 shareware (id Software, freely
# redistributable) + GLQuake 0.97 with the 3dfx MiniGL for Voodoo Graphics
# (opengl32.dll, imports glide2x.dll). Not in the repository; unpacked to
# build/compat/glquake/quake/.
#
# The installers are NOT executed: quake106.zip contains resource.1 (LHA
# -lh5-, tools/compat/unpack/lh5.py), glq1114.exe holds its files as SZDD
# (tools/compat/unpack/szdd.py). Checksums are verified.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/build/compat/glquake
U=$ROOT/tools/compat/unpack
mkdir -p "$D/dl"
cd "$D/dl"
[ -f quake106.zip ] || curl -sSfL -o quake106.zip https://cdn.netbsd.org/pub/pkgsrc/distfiles/quake106.zip
[ -f glq1114.exe ] || curl -sSfL -o glq1114.exe \
    "https://web.archive.org/web/20170610193118/http://www.quaketerminus.com/nqexes/glq1114.exe"
echo "8cee4d03ee092909fdb6a4f84f0c1357  quake106.zip" | md5sum -c --quiet
rm -rf parts "$D/quake"
mkdir -p parts "$D/quake"
unzip -o -q quake106.zip resource.1
python3 -I "$U/lh5.py" resource.1 "$D/quake" >/dev/null
echo "5906e5998fc3d896ddaf5e6a62e03abb  $D/quake/ID1/PAK0.PAK" | md5sum -c --quiet
python3 -I "$U/szdd.py" glq1114.exe parts >/dev/null
# part0 = opengl32.dll (3dfx MiniGL), part1 = driver readme, part2 = glquake.exe,
# part3 = GLQuake readme
objdump -p parts/part0.exe 2>/dev/null | grep -q "glide2x.dll" || { echo "MiniGL not found" >&2; exit 1; }
cp parts/part0.exe "$D/quake/opengl32.dll"
cp parts/part2.exe "$D/quake/glquake.exe"
cp parts/part1.txt "$D/quake/GLDRV.TXT"
cp parts/part3.txt "$D/quake/GLQREAD.TXT"
rm -f resource.1
echo "GLQuake in $D/quake"
