"""Minimal XWD reader (ZPixmap, 24/32 bit, as produced by `xwd -root` on
Xvfb), without ImageMagick/PIL. Returns (width, height, pixel(x,y)->(r,g,b))."""
import struct


def load(path):
    d = open(path, "rb").read()
    hsize = struct.unpack_from(">I", d, 0)[0]
    (_, _, fmt, depth, w, h, _, byte_order, _, _, _, bpp, bpl,
     _, rmask, gmask, bmask, _, ncolors) = struct.unpack_from(">19I", d, 0)[:19]
    if fmt != 2 or bpp not in (24, 32):
        raise ValueError("only ZPixmap 24/32 bpp supported (fmt=%d bpp=%d)" % (fmt, bpp))
    off = hsize + ncolors * 12
    bypp = bpp // 8
    endian = "<" if byte_order == 0 else ">"

    def shift(mask):
        s = 0
        while mask and not (mask >> s) & 1:
            s += 1
        return s

    rs, gs, bs = shift(rmask), shift(gmask), shift(bmask)

    def px(x, y):
        p = off + y * bpl + x * bypp
        v = int.from_bytes(d[p:p + bypp], "little" if endian == "<" else "big")
        return ((v & rmask) >> rs, (v & gmask) >> gs, (v & bmask) >> bs)

    return w, h, px


def to_ppm(path, out):
    w, h, px = load(path)
    with open(out, "wb") as f:
        f.write(b"P6 %d %d 255\n" % (w, h))
        f.write(bytes(c for y in range(h) for x in range(w) for c in px(x, y)))


def to_png(path, out, crop=None):
    """crop = (x, y, w, h) or None. zlib only, no PIL."""
    import zlib
    w, h, px = load(path)
    x0, y0, cw, ch = crop or (0, 0, w, h)
    raw = b"".join(b"\0" + bytes(c for x in range(x0, x0 + cw) for c in px(x, y))
                   for y in range(y0, y0 + ch))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", cw, ch, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
