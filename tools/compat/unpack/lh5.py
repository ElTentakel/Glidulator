# Minimal LHA -lh5- unpacker (level-0 headers) for the Quake shareware's
# resource.1. Usage: python3 lh5.py <archive> <target dir>; finds every -lh5-
# entry in the archive and unpacks it with its path.
import sys, re, struct, os
NC, NP, NT, CBIT, PBIT, TBIT = 510, 14, 19, 9, 4, 5

class Bits:
    def __init__(s, d, pos): s.d, s.p, s.buf, s.n = d, pos, 0, 0
    def fill(s, k):
        while s.n < k:
            b = s.d[s.p] if s.p < len(s.d) else 0
            s.p += 1; s.buf = ((s.buf << 8) | b) & ((1 << 64) - 1); s.n += 8
    def peek(s, k): s.fill(k); return (s.buf >> (s.n - k)) & ((1 << k) - 1)
    def skip(s, k): s.fill(k); s.n -= k
    def get(s, k):
        if k == 0: return 0
        v = s.peek(k); s.n -= k; return v

def table(lens, nsym):
    # canonical: by length, then symbol; 16-bit lookup
    t = [0] * 65536; code = 0
    for L in range(1, 17):
        for sym in range(nsym):
            if lens[sym] == L:
                span = 1 << (16 - L); start = code << (16 - L)
                t[start:start + span] = [sym] * span; code += 1
        code <<= 1
    return t

def read_pt(b, nn, nbit, special):
    n = b.get(nbit)
    if n == 0:
        c = b.get(nbit); return [0] * nn, [c] * 65536
    lens = [0] * nn; i = 0
    while i < n:
        c = b.peek(3)
        if c == 7:
            v = b.peek(16); mask = 1 << 12
            while v & mask: mask >>= 1; c += 1
            b.skip(c - 3)
        else:
            b.skip(3)
        lens[i] = c; i += 1
        if i == special:
            k = b.get(2)
            for _ in range(k): lens[i] = 0; i += 1
    return lens, table(lens, nn)

def read_c(b, ptl, ptt):
    n = b.get(CBIT)
    if n == 0:
        c = b.get(CBIT); return [0] * NC, [c] * 65536
    lens = [0] * NC; i = 0
    while i < n:
        c = ptt[b.peek(16)]; b.skip(ptl[c])
        if c <= 2:
            k = 1 if c == 0 else (b.get(4) + 3 if c == 1 else b.get(CBIT) + 20)
            for _ in range(k): lens[i] = 0; i += 1
        else:
            lens[i] = c - 2; i += 1
    return lens, table(lens, NC)

def lh5(d, pos, size):
    b = Bits(d, pos); out = bytearray(); left = 0
    while len(out) < size:
        if left == 0:
            left = b.get(16)
            ptl, ptt = read_pt(b, NT, TBIT, 3)
            cl, ct = read_c(b, ptl, ptt)
            pl, pt = read_pt(b, NP, PBIT, -1)
        left -= 1
        c = ct[b.peek(16)]; b.skip(cl[c])
        if c < 256:
            out.append(c)
        else:
            ln = c - 256 + 3
            p = pt[b.peek(16)]; b.skip(pl[p])
            if p: p = (1 << (p - 1)) + b.get(p - 1)
            st = len(out) - p - 1
            if p + 1 >= ln: out += out[st:st + ln]
            else:
                for k in range(ln): out.append(out[st + k])
    return bytes(out[:size])

src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
for m in re.finditer(rb'-lh5-', d):
    s = m.start() - 2; hs = d[s]
    comp, orig = struct.unpack('<II', d[s + 7:s + 15]); nl = d[s + 21]
    name = d[s + 22:s + 22 + nl].decode('ascii').replace('\\', '/')
    data = lh5(d, s + 2 + hs, orig)
    p = os.path.join(dst, name); os.makedirs(os.path.dirname(p) or dst, exist_ok=True)
    open(p, 'wb').write(data); print(name, len(data), flush=True)
