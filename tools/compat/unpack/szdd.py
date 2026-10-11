# SZDD unpacker (MS "compress", LZ77 with 4 KB window) for the files appended
# to the GLQuake installer glq1114.exe: python3 szdd.py <file> <target dir>
# writes part<n>.exe/.txt (n = order in the installer).
import sys, re, struct
def szdd(d):
    assert d[:8] == b'SZDD\x88\xf0\x27\x33'
    size = struct.unpack('<I', d[10:14])[0]
    win = bytearray(b' ' * 4096); pos = 4096 - 16; out = bytearray(); i = 14
    while len(out) < size and i < len(d):
        flags = d[i]; i += 1
        for b in range(8):
            if len(out) >= size or i >= len(d): break
            if flags & (1 << b):
                c = d[i]; i += 1; out.append(c); win[pos] = c; pos = (pos + 1) & 4095
            else:
                lo, hi = d[i], d[i+1]; i += 2
                off = lo | ((hi & 0xf0) << 4); ln = (hi & 0x0f) + 3
                for k in range(ln):
                    c = win[(off + k) & 4095]; out.append(c); win[pos] = c; pos = (pos + 1) & 4095
    return bytes(out[:size]), i
src, outdir = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
for n, m in enumerate(re.finditer(b'SZDD\x88\xf0\x27\x33', d)):
    data, _ = szdd(d[m.start():])
    kind = 'exe' if data[:2] == b'MZ' else 'txt'
    p = f'{outdir}/part{n}.{kind}'; open(p, 'wb').write(data); print(p, len(data))
