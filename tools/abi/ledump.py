#!/usr/bin/env python3
"""Minimal LE (linear executable) reader for DOS/4G DLLs and EXEs.

Reads the MZ stub, the LE header, the object table and the resident /
non-resident name tables. Written from the public LE/LX format
description; used by check_exports.py and the loader tests.
"""
import struct
import sys


class LEError(Exception):
    pass


class LEModule:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise LEError("no MZ header")
        self.le_off = struct.unpack_from("<I", data, 0x3C)[0]
        h = self.le_off
        if data[h:h + 2] != b"LE":
            raise LEError("no LE header at %#x" % h)
        u = lambda o: struct.unpack_from("<I", data, h + o)[0]
        self.module_flags = u(0x10)
        self.num_pages = u(0x14)
        self.eip_object, self.eip = u(0x18), u(0x1C)
        self.esp_object, self.esp = u(0x20), u(0x24)
        self.page_size = u(0x28)
        self.last_page_size = u(0x2C)
        self.fixup_size = u(0x30)
        self.obj_table = h + u(0x40)
        self.num_objects = u(0x44)
        self.page_map = h + u(0x48)
        self.res_names = h + u(0x58)
        self.entry_table = h + u(0x5C)
        self.fixup_page_table = h + u(0x68)
        self.fixup_record_table = h + u(0x6C)
        self.import_modules = h + u(0x70)
        self.num_import_modules = u(0x74)
        self.data_pages = u(0x80)
        self.nonres_names = u(0x88)
        self.nonres_names_len = u(0x8C)
        self.objects = []
        for i in range(self.num_objects):
            o = self.obj_table + 24 * i
            size, base, flags, pmi, pmc, _ = struct.unpack_from("<6I", data, o)
            self.objects.append(dict(size=size, base=base, flags=flags,
                                     page_index=pmi, page_count=pmc))

    @staticmethod
    def _names(data, off, end=None):
        out = []
        while end is None or off < end:
            n = data[off]
            if n == 0:
                break
            name = data[off + 1:off + 1 + n].decode("latin-1")
            ordinal = struct.unpack_from("<H", data, off + 1 + n)[0]
            out.append((name, ordinal))
            off += 3 + n
        return out

    def resident_names(self):
        return self._names(self.data, self.res_names)

    def nonresident_names(self):
        if not self.nonres_names:
            return []
        return self._names(self.data, self.nonres_names,
                           self.nonres_names + self.nonres_names_len)

    def module_name(self):
        names = self.resident_names()
        return names[0][0] if names and names[0][1] == 0 else None

    def exports(self):
        """Exported names (ordinal != 0) from both name tables."""
        return sorted({n for n, o in self.resident_names() + self.nonresident_names() if o})

    def is_library(self):
        return bool(self.module_flags & 0x8000)


def main(argv):
    for path in argv[1:]:
        m = LEModule(open(path, "rb").read())
        print("%s: module=%s flags=%#x objects=%d imports=%d exports=%d" % (
            path, m.module_name(), m.module_flags, m.num_objects,
            m.num_import_modules, len(m.exports())))
        for i, o in enumerate(m.objects, 1):
            print("  object %d: size=%#x base=%#x flags=%#x pages=%d@%d" % (
                i, o["size"], o["base"], o["flags"], o["page_count"], o["page_index"]))
        for n in m.exports():
            print("  export", n)


if __name__ == "__main__":
    main(sys.argv)
