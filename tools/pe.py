"""Minimal PE32 loader: maps sections at their RVAs and reads the import table."""
import struct


class Section:
    __slots__ = ("name", "va", "vsize", "raw_off", "raw_size", "flags")

    def __init__(self, name, va, vsize, raw_off, raw_size, flags):
        self.name, self.va, self.vsize = name, va, vsize
        self.raw_off, self.raw_size, self.flags = raw_off, raw_size, flags

    @property
    def executable(self):
        return bool(self.flags & 0x20000000)


class PE:
    def __init__(self, path):
        b = open(path, "rb").read()
        self.raw = b
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        assert b[pe:pe + 4] == b"PE\0\0"
        self.machine, nsec, self.timestamp, _, _, optsz, self.chars = struct.unpack_from("<HHIIIHH", b, pe + 4)
        opt = pe + 24
        self.entry_rva = struct.unpack_from("<I", b, opt + 16)[0]
        self.base = struct.unpack_from("<I", b, opt + 28)[0]
        self.image_size = struct.unpack_from("<I", b, opt + 56)[0]
        self.headers_size = struct.unpack_from("<I", b, opt + 60)[0]
        dd = opt + 96
        self.dirs = [struct.unpack_from("<II", b, dd + 8 * i) for i in range(16)]
        self.sections = []
        so = opt + optsz
        for i in range(nsec):
            name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", b, so + 40 * i)
            flags = struct.unpack_from("<I", b, so + 40 * i + 36)[0]
            self.sections.append(Section(name.rstrip(b"\0").decode(errors="replace"), va, vsz, rptr, rsz, flags))
        img = bytearray(self.image_size)
        img[:self.headers_size] = b[:self.headers_size]
        for s in self.sections:
            n = min(s.raw_size, s.vsize) if s.vsize else s.raw_size
            img[s.va:s.va + n] = b[s.raw_off:s.raw_off + n]
        self.image = bytes(img)
        self.imports = self._imports()  # {iat_va: (dll, name)}

    @property
    def entry(self):
        return self.base + self.entry_rva

    def section_of(self, va):
        rva = va - self.base
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vsize, s.raw_size):
                return s
        return None

    def u32(self, va):
        return struct.unpack_from("<I", self.image, va - self.base)[0]

    def _imports(self):
        out = {}
        rva, size = self.dirs[1]
        if not rva:
            return out
        img = self.image
        o = rva
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", img, o)
            if not name_rva:
                break
            dll = img[name_rva:img.index(b"\0", name_rva)].decode()
            t = oft or ft
            slot = ft
            while True:
                v = struct.unpack_from("<I", img, t)[0]
                if not v:
                    break
                if v & 0x80000000:
                    name = f"#{v & 0xFFFF}"
                else:
                    name = img[v + 2:img.index(b"\0", v + 2)].decode()
                out[self.base + slot] = (dll, name)
                t += 4
                slot += 4
            o += 20
        return out
