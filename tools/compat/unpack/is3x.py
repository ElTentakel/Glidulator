#!/usr/bin/env python3
"""Extract InstallShield 3 launcher SFX / .Z archives without running them.

  is3x.py <setup.exe> <outdir>

Launcher container: entries of u32 n, n bytes (obfuscated name), u32 ?,
u32 size, data. Entries that start with 0x8C655D13 are IS3 Z archives,
whose files are PKWARE DCL imploded (decoder below follows zlib's blast.c).
"""
import os
import struct
import sys

# ---------- PKWARE DCL explode (port of zlib contrib/blast) ----------
MAXBITS = 13


class Huff:
    def __init__(self, rep):
        lengths = []
        for b in rep:
            lengths += [(b & 15) + 0] * ((b >> 4) + 1)
        n = len(lengths)
        self.count = [0] * (MAXBITS + 1)
        for l in lengths:
            self.count[l] += 1
        offs = [0] * (MAXBITS + 1)
        for l in range(1, MAXBITS):
            offs[l + 1] = offs[l] + self.count[l]
        self.symbol = [0] * n
        for s in range(n):
            if lengths[s]:
                self.symbol[offs[lengths[s]]] = s
                offs[lengths[s]] += 1


LITLEN = [11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
          9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
          7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
          8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
          44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
          44, 173]
LENLEN = [2, 35, 36, 53, 38, 23]
DISTLEN = [2, 20, 53, 230, 247, 151, 248]
BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]
H_LIT, H_LEN, H_DIST = Huff(LITLEN), Huff(LENLEN), Huff(DISTLEN)


def explode(src):
    pos = 0
    bitbuf = 0
    bitcnt = 0
    out = bytearray()

    def bits(need):
        nonlocal pos, bitbuf, bitcnt
        val = bitbuf
        while bitcnt < need:
            val |= src[pos] << bitcnt
            pos += 1
            bitcnt += 8
        bitbuf = val >> need
        bitcnt -= need
        return val & ((1 << need) - 1)

    def decode(h):
        code = first = index = 0
        for l in range(1, MAXBITS + 1):
            code |= bits(1) ^ 1
            cnt = h.count[l]
            if code < first + cnt:
                return h.symbol[index + (code - first)]
            index += cnt
            first += cnt
            first <<= 1
            code <<= 1
        raise ValueError("bad code")

    lit = bits(8)
    dictbits = bits(8)
    if lit > 1 or not 4 <= dictbits <= 6:
        raise ValueError("bad DCL header")
    while True:
        if bits(1):
            sym = decode(H_LEN)
            ln = BASE[sym] + bits(EXTRA[sym])
            if ln == 519:
                break
            sym = 2 if ln == 2 else dictbits
            dist = (decode(H_DIST) << sym) + bits(sym) + 1
            for _ in range(ln):
                out.append(out[-dist])
        else:
            out.append(decode(H_LIT) if lit else bits(8))
    return bytes(out)


# ---------- IS3 Z archive ----------
def z_archive(d, outdir):
    cnt = struct.unpack_from('<H', d, 0x0c)[0]
    dir_off = struct.unpack_from('<I', d, 0x29)[0]
    ndirs = struct.unpack_from('<H', d, 0x31)[0]
    ft_off = struct.unpack_from('<I', d, 0x33)[0]
    dirs = []
    p = dir_off
    for _ in range(ndirs):
        nfiles, elen, nlen = struct.unpack_from('<HHH', d, p)
        dirs.append(d[p + 6:p + 6 + nlen].decode('latin1'))
        p += elen
    p = ft_off
    for _ in range(cnt):
        di = struct.unpack_from('<H', d, p + 1)[0]
        usize, csize, off = struct.unpack_from('<III', d, p + 3)
        elen = struct.unpack_from('<H', d, p + 0x17)[0]
        nlen = d[p + 0x1d]
        name = d[p + 0x1e:p + 0x1e + nlen].decode('latin1')
        sub = dirs[di] if di < len(dirs) else ''
        rel = os.path.join(sub.replace('\\', '/'), name)
        if '..' in rel.split('/'):
            raise ValueError('bad path ' + rel)
        data = explode(d[off:off + csize])
        if len(data) != usize:
            print('  size mismatch', rel, len(data), usize)
        dst = os.path.join(outdir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(data)
        print('  %-40s %8d' % (rel, usize))
        p += elen


def main():
    d = open(sys.argv[1], 'rb').read()
    outdir = sys.argv[2]
    p = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xcc00
    k = 0
    while p + 12 < len(d):
        n = struct.unpack_from('<I', d, p)[0]
        if n > 300:
            break
        sz = struct.unpack_from('<I', d, p + 8 + n)[0]
        data = d[p + 12 + n:p + 12 + n + sz]
        sub = os.path.join(outdir, 'part%02d' % k)
        os.makedirs(sub, exist_ok=True)
        if data[:4] == b'\x13\x5d\x65\x8c':
            print('Z archive part%02d (%d bytes)' % (k, sz))
            z_archive(data, sub)
        else:
            open(os.path.join(sub, 'raw.bin'), 'wb').write(data)
        p += 12 + n + sz
        k += 1


if __name__ == '__main__':
    main()
