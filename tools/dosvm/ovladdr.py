#!/usr/bin/env python3
"""Map a fault address of the OVL to a function.

    tools/dosvm/ovladdr.py <GLIDE2X.map> <address of grGlideInit> <address>...
    tools/dosvm/ovladdr.py <GLIDE2X.map> <GLIDULAT.LOG>   (every EIP in the log)

The debug OVL logs "Adresse grGlideInit: <decimal>"; fault addresses
("Multi-core: exception N ..., stack <EIP> ...") are hexadecimal. Output:
module, nearest public function before it and the distance (the map does
not list static functions; then the location is in a static function of
the same module after the one named). Use the map of exactly the OVL that
produced the log."""
import re, sys

def num(s):
    return int(s, 16) if re.search(r"[a-fA-F]|^0x", s) or len(s) == 8 else int(s)

mp = sys.argv[1]
if len(sys.argv) == 3 and not re.fullmatch(r"(0x)?[0-9a-fA-F]+", sys.argv[2]):
    log = open(sys.argv[2], errors="replace").read()
    init = int(re.search(r"Address grGlideInit: (\d+)", log).group(1))
    addrs = re.findall(r"EIP ([0-9A-F]{8})", log)
    addrs += re.findall(r"exception \d+ on an AP, stack ([0-9A-F]{8})", log)
    addrs = list(dict.fromkeys(addrs))
else:
    init, addrs = num(sys.argv[2]), sys.argv[3:]
syms, module = [], "?"
for line in open(mp, errors="replace"):
    m = re.match(r"Module: (\S+?)\(", line)
    if m:
        module = m.group(1).split("/")[-1]
        continue
    m = re.match(r"0001:([0-9a-f]{8})[+*]?\s+(\S+)", line)
    if m:
        syms.append((int(m.group(1), 16), m.group(2), module))
syms.sort()
base = init - next(o for o, n, _ in syms if n == "_grGlideInit@0")
print("OVL-Code ab %08X" % base)
for a in addrs:
    off = num(a) - base
    best = max((s for s in syms if s[0] <= off), default=None)
    if best is None or off > syms[-1][0] + 0x100000:
        print("%s: outside the OVL (offset %X)" % (a, off))
    else:
        print("%s: Offset %05X = %s + %X (%s)" % (a, off, best[1], off - best[0], best[2]))
        nxt = min((s for s in syms if s[0] > off), default=None)
        if nxt and nxt[2] != best[2]:
            print("    before %s (%s, %X further): rather a static function at the start of %s"
                  % (nxt[1], nxt[2], nxt[0] - off, nxt[2]))
