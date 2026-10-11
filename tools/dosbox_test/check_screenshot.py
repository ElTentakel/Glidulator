"""Checks a DOSBox run of OVLTEST.EXE (see run.sh):

1. OVLTEST.LOG: the program completed, rasterizer pixels are right, the
   expected video mode was set (vbe: VBE mode 320x240, mode13: mode 13h).
2. Screenshots: there is a picture where the DOSBox window has the mode
   size (or an integer multiple, see mode_scale) and shows the test picture:
   dark blue background, flat red on the left, Gouraud on the right with red
   top, green left and blue right corner (coordinates in the 320x240 source
   grid, converted to the window size; mode 13h is stretched to 320x200).
3. Afterwards the window has the text mode size from before the test again
   (grGlideShutdown restored the old mode).

Saves the matching picture as frame.png in the run directory."""
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xwd  # noqa: E402

EXPECT_SIZE = {"vbe": (320, 240), "mode13": (320, 200)}


def mode_scale(geo, ew, eh):
    """Factor k if the window is k times the mode size, else 0. DOSBox shows
    mode 13h doubled on machine=vgaonly (640x400, VGA double scan), but 1:1
    on svga_s3."""
    if not geo:
        return 0
    for k in (1, 2, 3):
        if geo[:2] == (ew * k, eh * k):
            return k
    return 0


def geometry(path):
    try:
        m = re.search(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", open(path).read())
    except OSError:
        return None
    return tuple(int(v) for v in m.groups()) if m else None


def dominant(rgb, ch):
    """Channel ch clearly stronger than the other two."""
    others = [rgb[i] for i in range(3) if i != ch]
    return rgb[ch] > 120 and all(rgb[ch] > o + 60 for o in others)


# Background 0x000040 (blue 66 after 565->8 bit) through the output gamma
# table: gamma 1.0 -> 66 (VBE 565: 64), 1.3 (default) -> ~90 (88). Mode
# 13h dithers between the palette's blue levels 48 and 100, so the
# background is checked as the mean of a window (see check_frame).
BG_BLUE = {("vbe", "1.0"): (56, 72), ("vbe", "1.3"): (80, 100),
           ("mode13", "1.0"): (54, 78), ("mode13", "1.3"): (76, 100)}
g_bg_range = (40, 130)


def is_bg(rgb):
    r, g, b = rgb
    return r < 24 and g < 24 and g_bg_range[0] <= b <= g_bg_range[1]


# (name, x, y in the 320x240 source grid, check)
SAMPLES = [
    ("background top left", 5, 5, is_bg),
    ("background centre", 160, 120, is_bg),
    ("red triangle", 60, 180, lambda c: dominant(c, 0)),
    ("Gouraud apex (red)", 235, 40, lambda c: dominant(c, 0)),
    ("Gouraud bottom left (green)", 182, 212, lambda c: dominant(c, 1)),
    ("Gouraud bottom right (blue)", 286, 212, lambda c: dominant(c, 2)),
]


def check_frame(shot, geo):
    w, h, x0, y0 = geo
    _, _, px = xwd.load(shot)
    bad = []
    for name, sx, sy, ok in SAMPLES:
        cx, cy = x0 + sx * w // 320, y0 + sy * h // 240
        if ok is is_bg:  # flat area: mean of 16x16 (dithered on mode 13h)
            cs = [px(cx + i, cy + j) for j in range(16) for i in range(16)]
            c = tuple(sum(v[k] for v in cs) // len(cs) for k in range(3))
        else:
            c = px(cx, cy)
        if not ok(c):
            bad.append("%s: %s" % (name, c))
    return bad


def main(run, expect, gamma="1.3"):
    global g_bg_range
    g_bg_range = BG_BLUE.get((expect, gamma), (76, 100))
    errors = []
    log_path = os.path.join(run, "OVLTEST.LOG")
    log = open(log_path).read() if os.path.exists(log_path) else ""
    print("--- %s ---\n%s" % (os.path.basename(run), log.strip() or "(no OVLTEST.LOG)"))
    if "OVLTEST END" not in log:
        errors.append("OVLTEST did not run to the end")
    if "peek bg=0008 red=f800" not in log:
        errors.append("rasterizer pixels wrong")
    if "3df 256x128 mem=65536 wrong=0" not in log:
        errors.append(".3DF file not read correctly through the OVL (DOS file access)")
    ew, eh = EXPECT_SIZE[expect]
    if expect == "mode13" and "Mode 0x013 320x200x8" not in log:
        errors.append("mode 13h not set")
    if expect == "vbe" and not re.search(r"Mode 0x1[0-9a-f]{2} %dx%dx(15|16|24|32)" % (ew, eh), log):
        errors.append("no VBE mode %dx%d set" % (ew, eh))

    shots = sorted(glob.glob(os.path.join(run, "shot*.xwd")))
    geos = [geometry(s[:-4] + ".geo") for s in shots]

    found = None
    last_bad = []
    for i, (shot, geo) in enumerate(zip(shots, geos)):
        if mode_scale(geo, ew, eh):
            try:
                bad = check_frame(shot, geo)
            except (ValueError, struct.error, IndexError, OSError):
                # xwd occasionally writes empty/truncated files while DOSBox
                # is exiting; skip them.
                continue
            if not bad:
                found = i
                xwd.to_png(shot, os.path.join(run, "frame.png"), (geo[2], geo[3], geo[0], geo[1]))
                break
            last_bad = bad
    if found is None:
        errors.append("no screenshot with the correct test picture in %dx%d (or a multiple)" % (ew, eh))
        errors.extend("  " + b for b in last_bad)
    else:
        print("test picture correct on screen: %s" % os.path.basename(shots[found]))
        # Text mode = the window size right before the graphics picture (not
        # the very first one: DOSBox briefly opens with 640x400 at start,
        # which on vgaonly happens to equal the doubled mode 13h size).
        gfx = geos[found][:2]
        before = [g for g in geos[:found] if g and g[:2] != gfx]
        later = [g for g in geos[found + 1:] if g]
        if before and later and later[-1][:2] == before[-1][:2]:
            print("text mode restored (window back to %dx%d)" % before[-1][:2])
        else:
            errors.append("window not back to text mode size after the test (%s -> %s)"
                          % (before[-1] if before else None, later[-1] if later else None))

    if errors:
        print("FAILED:\n  " + "\n  ".join(errors))
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:4]))
