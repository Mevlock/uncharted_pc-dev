"""Minimal PSARC reader: list / extract. usage: psarc.py list|extract ARCHIVE [PATTERN] [OUTDIR]"""
import struct, sys, zlib, os, fnmatch, lzma


class Psarc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(32)
        magic, ver_major, ver_minor, comp, toc_len, entry_size, entries, block, flags = struct.unpack(
            '>4sHH4sIIIII', h)
        assert magic == b'PSAR', magic
        self.comp, self.block = comp, block
        toc = self.f.read(toc_len - 32)
        self.entries = []
        for i in range(entries):
            e = toc[i * entry_size:(i + 1) * entry_size]
            md5 = e[:16]
            zindex = struct.unpack('>I', e[16:20])[0]
            size = int.from_bytes(e[20:25], 'big')
            offset = int.from_bytes(e[25:30], 'big')
            self.entries.append((zindex, size, offset))
        # block size table
        btab = toc[entries * entry_size:]
        width = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
        self.zsizes = [int.from_bytes(btab[i:i + width], 'big') for i in range(0, len(btab) - width + 1, width)]
        names = self.read(0).decode('utf-8', 'replace').split('\n')
        self.files = dict(zip(names, self.entries[1:]))
        if flags & 2:  # absolute paths
            pass

    def read(self, idx_or_entry):
        zindex, size, offset = self.entries[idx_or_entry] if isinstance(idx_or_entry, int) else idx_or_entry
        out = bytearray()
        self.f.seek(offset)
        while len(out) < size:
            zs = self.zsizes[zindex]
            chunk = self.f.read(zs if zs else self.block)
            if zs == 0 or zs == min(self.block, size - len(out)) and not chunk[:2] in (b'\x78\xda', b'\x78\x9c', b'\x78\x01', b'\x78\x5e'):
                out += chunk
            else:
                try:
                    out += zlib.decompress(chunk)
                except zlib.error:
                    try:
                        out += lzma.decompress(chunk)
                    except Exception:
                        out += chunk
            zindex += 1
        return bytes(out[:size])


if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    pat = sys.argv[3] if len(sys.argv) > 3 else '*'
    a = Psarc(path)
    if cmd == 'list':
        for n, (z, s, o) in a.files.items():
            if fnmatch.fnmatch(n.lower(), pat.lower()):
                print(s, n)
    else:
        outdir = sys.argv[4]
        for n, e in a.files.items():
            if fnmatch.fnmatch(n.lower(), pat.lower()):
                dst = os.path.join(outdir, n.lstrip('/'))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, 'wb').write(a.read(e))
                print('wrote', dst)
