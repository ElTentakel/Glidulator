"""DOSBox on Xvfb with scripted keyboard input (XTest via ctypes, no xdotool
needed) and screenshots, for programs with interactive menus (demos/games
that ask for options before starting).

  session.py <run-directory> <script>

The run directory must contain dosbox.conf. Script: one action per line,
'#' = comment:
  wait <seconds>
  key <keysym> [...]     e.g. key Right, key Return, key y, key Escape
  shot <name>            screenshot of the DOSBox window -> <name>.png
  shots <name> <n> <s>   n screenshots, s seconds apart
  mouseto <fx> <fy>      mouse pointer to a fraction of window width/height
                         (0..1), e.g. mouseto 0.5 0.9
  click [button]         press/release a mouse button (1 = left, default)
Needs autolock=false in dosbox.conf: DOSBox 0.74 then sets the DOS mouse
ABSOLUTELY to the pointer position in the window (a captured mouse only
gets relative motion, which XTest cannot produce reliably).
"""
import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xwd  # noqa: E402

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]


def geometry(disp):
    out = subprocess.run(["xwininfo", "-root", "-tree"], env=dict(os.environ, DISPLAY=disp),
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if '"DOSBox' in line:
            m = re.search(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", line)
            if m:
                return tuple(int(v) for v in m.groups())
    return None


def main(run, script):
    disp_no = 90
    while os.path.exists("/tmp/.X11-unix/X%d" % disp_no) or os.path.exists("/tmp/.X%d-lock" % disp_no):
        disp_no += 1
    disp = ":%d" % disp_no
    xvfb = subprocess.Popen(["Xvfb", disp, "-screen", "0", "1024x768x24"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    env = dict(os.environ, DISPLAY=disp, SDL_AUDIODRIVER="dummy")
    box = subprocess.Popen(["dosbox", "-conf", "dosbox.conf", "-noconsole"], cwd=run, env=env,
                           stdout=open(os.path.join(run, "dosbox.out"), "w"), stderr=subprocess.STDOUT)
    dpy = x11.XOpenDisplay(disp.encode())
    try:
        for raw in open(script):
            line = raw.split("#", 1)[0].split()
            if not line:
                continue
            cmd, args = line[0], line[1:]
            if box.poll() is not None:
                print("DOSBox beendet (rc=%s) vor: %s" % (box.returncode, " ".join(line)))
                break
            if cmd == "wait":
                time.sleep(float(args[0]))
            elif cmd == "key":
                g = geometry(disp)
                if g:  # pointer into the window: without a WM the focus follows the pointer
                    xtst.XTestFakeMotionEvent(dpy, -1, g[2] + g[0] // 2, g[3] + g[1] // 2, 0)
                for k in args:
                    code = x11.XKeysymToKeycode(dpy, x11.XStringToKeysym(k.encode()))
                    xtst.XTestFakeKeyEvent(dpy, code, 1, 0)
                    x11.XFlush(dpy)
                    time.sleep(0.08)
                    xtst.XTestFakeKeyEvent(dpy, code, 0, 0)
                    x11.XFlush(dpy)
                    time.sleep(0.25)
            elif cmd in ("mouseto", "click"):
                g = geometry(disp)
                if not g:
                    continue
                if cmd == "mouseto":
                    x = g[2] + int(float(args[0]) * (g[0] - 1))
                    y = g[3] + int(float(args[1]) * (g[1] - 1))
                    # in two steps so DOSBox certainly sees a motion event
                    xtst.XTestFakeMotionEvent(dpy, -1, x - 1, y - 1, 0)
                    x11.XFlush(dpy)
                    time.sleep(0.1)
                    xtst.XTestFakeMotionEvent(dpy, -1, x, y, 0)
                    x11.XFlush(dpy)
                    time.sleep(0.3)
                else:
                    b = int(args[0]) if args else 1
                    xtst.XTestFakeButtonEvent(dpy, b, 1, 0)
                    x11.XFlush(dpy)
                    time.sleep(0.15)
                    xtst.XTestFakeButtonEvent(dpy, b, 0, 0)
                    x11.XFlush(dpy)
                    time.sleep(0.3)
            elif cmd in ("shot", "shots"):
                n, every = (int(args[1]), float(args[2])) if cmd == "shots" else (1, 0)
                for i in range(n):
                    g = geometry(disp)
                    name = args[0] if n == 1 else "%s_%02d" % (args[0], i)
                    tmp = os.path.join(run, name + ".xwd")
                    subprocess.run(["xwd", "-root", "-silent", "-out", tmp], env=env)
                    if g:
                        xwd.to_png(tmp, os.path.join(run, name + ".png"), (g[2], g[3], g[0], g[1]))
                        print("%s: %dx%d" % (name, g[0], g[1]))
                    os.remove(tmp)
                    if i + 1 < n:
                        time.sleep(every)
            else:
                raise SystemExit("unbekannte Aktion: " + cmd)
    finally:
        if box.poll() is None:
            box.terminate()
            box.wait()
        xvfb.terminate()


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
