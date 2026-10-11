# Shared table of the DOS Glide programs for run_demo.sh (headless) and
# play_demo.sh (visible), sourced with ".". demo_info <name> sets:
#   URL   path below https://files.scene.org/get/ or a full URL
#   ZIP   file name of the download (default: last part of URL)
#   CDIR  directory in the archive to start in (default ".")
#   CMD   start command in DOSBox (in CDIR)
#   MENU  operating hints for the start menu (play_demo.sh)
# The programs are not in the repository but downloaded to
# build/compat/<name>/.
DEMOS="dimension shortcut trip fucktory yume2 gta archdyn burnout"

# Game demos from the DOS Games Archive: download via file.php?id=<n>.
DGA=https://www.dosgamesarchive.com/file.php?id=

demo_info() {
    ZIP=
    CDIR=.
    case "$1" in
    dimension)
        URL=mirrors/hornet/demos/1998/d/dimen.zip
        CMD="DOS4GW DIMEN.EXE"
        MENU="Dimension (gOds 1998, Demo, 8:40 min)
   MIDAS sound menu: arrow keys + Enter. With sound: \"Sound Blaster 16\",
   then port 220h, IRQ 5, DMA 1 / HDMA 5 (the defaults usually fit);
   without sound: \"No Sound\" at the bottom. Then ~10 s loading." ;;
    shortcut)
        URL=mirrors/hornet/demos/1998/s/shortcut.zip
        CMD="DOS4GW SHORTCUT.EXE"
        MENU="Shortcut (Buckshag 1998, Demo)
   IMS sound setup: pick the device on the right (arrow keys), e.g.
   \"SoundBlaster 16\" or \"quiet\" (no sound), then Enter twice.
   Then about a minute of black screen (loading), then the Buckshag logo." ;;
    trip)
        URL=mirrors/hornet/demos/1998/t/trip.zip
        CMD="TRIP"
        MENU="Trip (Kosmic Free Music Foundation 1997, Demo)
   Text menu: 3 + Enter (3Dfx), 0 + Enter (640x480),
   sound: 1 + Enter (Sound Blaster) or 0 + Enter (no sound)." ;;
    fucktory)
        URL=parties/1998/juhla98/demo/fuctory.zip
        CMD="FUCKTORY"
        MENU="Fucktory (Capsicum Tincture 1998, Demo)
   MIDAS menu \"Current Configuration\": Enter = Accept (No Sound);
   for sound, change the sound card first. The long white fades are
   intended (the demo's white flash, very slow in DOSBox)." ;;
    yume2)
        URL=parties/1998/theparty98/demo/infy2rel.zip
        CMD="INFYUME2"
        MENU="Yume 2 (inf 1998, Demo)
   Crashes in DOSBox 0.74 before the first Glide call (not a
   Glidulator issue, see docs/compatibility.md)." ;;
    gta)
        URL=${DGA}3331
        ZIP=dl.zip
        CDIR=GTA3DFX/GTADOS
        CMD="DEMOFX"
        MENU="Grand Theft Auto (DMA Design 1997, 3Dfx game demo)
   Starts the 3Dfx version directly (DEMOFX.EXE). Controls: arrow keys,
   Ctrl = fire, Enter = get in/out of a car, Esc = menu." ;;
    archdyn)
        URL=${DGA}889
        ZIP=dl.zip
        CDIR=ADDEMO
        CMD="AD3DFX"
        MENU="Archimedean Dynasty (Massive/Blue Byte 1996, game demo with 3Dfx patch)
   AD3DFX.EXE starts the 3Dfx version (DESKTOP3/FIGHT3 load
   GLIDE2X.OVL; AD.EXE would be software). Mouse controls: click away the
   info and licence pages, then \"Instant Action\" in the main menu." ;;
    burnout)
        URL=${DGA}1547
        ZIP=dl.zip
        CDIR=BURNDEMO
        CMD="BURNOUT"
        MENU="Burnout: Championship Drag Racing (Bethesda 1998, game demo)
   BURNOUT.EXE is the 3Dfx version (BURNOUTS.EXE would be software).
   F7 toggles motion blur (3Dfx only)." ;;
    *)
        echo "unknown program: $1 (known: $DEMOS)" >&2
        return 1 ;;
    esac
    [ -n "$ZIP" ] || ZIP=$(basename "$URL")
}

# Downloads the program once to build/compat/<name>/zip/ and unpacks it
# afresh into $2 (run directory). demo_install_ovl then replaces every
# bundled GLIDE2X.OVL; a bundled DOS4GW.EXE stays (the path the program
# takes on real hardware), otherwise Open Watcom's is used.
demo_unpack() {
    _dir=$ROOT/build/compat/$1
    _zip=$_dir/zip/$ZIP
    mkdir -p "$_dir/zip"
    if [ ! -f "$_zip" ]; then
        echo "Downloading $1 ..."
        case "$URL" in
        http*) curl -sSL -A "Mozilla/5.0" -o "$_zip" "$URL" ;;
        *) curl -sSL -o "$_zip" "https://files.scene.org/get/$URL" ;;
        esac
    fi
    rm -rf "${2:?}"
    mkdir -p "$2"
    unzip -o -q "$_zip" -d "$2"
    [ -f "$2/$CDIR/DOS4GW.EXE" ] || cp /opt/watcom/binw/dos4gw.exe "$2/$CDIR/DOS4GW.EXE"
}

# Our OVL ($1) into the start directory and in place of every bundled
# GLIDE2X.OVL (some programs bring several, e.g. one for the Voodoo Rush);
# otherwise the program might load the original OVL.
demo_install_ovl() {
    cp "$1" "$2/$CDIR/GLIDE2X.OVL"
    find "$2" -iname glide2x.ovl | while read -r _ovl; do cp "$1" "$_ovl"; done
}

# Put a configuration file ($1, e.g. GLIDULAT.CFG/CAPTURE.CFG) into every
# directory of the run: the OVL reads it in the current directory, and some
# programs change elsewhere first (Fucktory: DATA).
demo_spread_file() {
    find "$2" -type d | while read -r _d; do [ "$_d/$(basename "$1")" = "$1" ] || cp "$1" "$_d/"; done
}
