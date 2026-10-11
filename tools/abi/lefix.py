#!/usr/bin/env python3
"""Post-link fix-ups that make GLIDE2X.OVL's header match the retail OVL.

wlink's `option nocaseexact` uppercases the module name, and `format os2 le
dll` sets module flags 0x8000; the retail runtime has module name "glide2x"
and flags 0x8200. Both are patched in place (same length, no layout change).

usage: lefix.py FILE [--modname glide2x] [--flags 0x8200]
"""
import argparse
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ledump import LEModule  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--modname", default="glide2x")
    ap.add_argument("--flags", default="0x8200")
    a = ap.parse_args()
    data = bytearray(open(a.file, "rb").read())
    m = LEModule(bytes(data))
    off = m.res_names
    n = data[off]
    cur = data[off + 1:off + 1 + n].decode("latin-1")
    if cur.lower() != a.modname.lower() or len(cur) != len(a.modname):
        sys.exit("lefix: module name %r cannot be replaced by %r" % (cur, a.modname))
    data[off + 1:off + 1 + n] = a.modname.encode()
    struct.pack_into("<I", data, m.le_off + 0x10, int(a.flags, 0))
    open(a.file, "wb").write(data)
    print("lefix: %s module=%s flags=%s" % (a.file, a.modname, a.flags))


if __name__ == "__main__":
    main()
