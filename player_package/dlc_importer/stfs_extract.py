"""Extract an Xbox 360 STFS package (CON/LIVE/PIRS), e.g. Trials Evolution DLC.

Usage: python tools/stfs_extract.py <package> <output dir> [--list]

Format notes (free60 / X360 docs): big-endian header; header size u32 at 0x340; volume
descriptor at 0x379; data is 0x1000-byte blocks with hash tables interleaved every 0xAA blocks,
so logical block numbers are mapped to physical offsets. Files marked consecutive are read block
by block through that mapping; others follow the next-block chain in the level-0 hash tables.
"""
import os
import struct
import sys


class STFS:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0xA000)
        if head[:4] not in (b"LIVE", b"PIRS", b"CON "):
            raise ValueError("not an STFS package")
        self.header_size = struct.unpack_from(">I", head, 0x340)[0]
        vd = head[0x379:0x379 + 0x24]
        self.block_sep = vd[2]
        self.ft_blocks = struct.unpack_from("<H", vd, 3)[0]
        self.ft_start = int.from_bytes(vd[5:8], "little")
        self.total_blocks = struct.unpack_from(">I", vd, 0x1C)[0]
        self.data_base = (self.header_size + 0xFFF) & ~0xFFF
        self.shift = 0 if self.block_sep & 1 else 1   # verified on Trials DLC (sep=1 -> 0)
        self.step = (0xAB, 0x718F) if self.shift == 0 else (0xAC, 0x723A)
        self.display_name = head[0x411:0x411 + 0x80].decode("utf-16-be").split("\0")[0]

    def phys(self, block):
        """Logical data block -> physical block index (hash tables interleaved)."""
        base = ((block + 0xAA) // 0xAA) << self.shift
        ret = base + block
        if block >= 0xAA:
            ret += ((block + 0x70E4) // 0x70E4) << self.shift
            if block >= 0x70E4:
                ret += ((block + 0x4AF768) // 0x4AF768) << self.shift
        return ret

    def read_block(self, block):
        self.f.seek(self.data_base + (self.phys(block) << 12))
        return self.f.read(0x1000)

    def level0_hash_block(self, block):
        if block < 0xAA:
            return 0
        num = (block // 0xAA) * self.step[0]
        num += ((block // 0x70E4) + 1) << self.shift
        if block // 0x70E4 == 0:
            return num
        return num + (1 << self.shift)

    def verify(self):
        """Data blocks whose SHA-1 doesn't match their level-0 hash table entry (a damaged dump)."""
        import hashlib
        bad = []
        for block in range(self.total_blocks):
            digest = hashlib.sha1(self.read_block(block)).digest()
            hb = self.level0_hash_block(block)
            # Packages saved more than once (shift 1) keep two copies of each hash table; the
            # second one is current for blocks rewritten since. A block is good if either matches.
            copies = (hb, hb + 1) if self.shift else (hb,)
            for table in copies:
                self.f.seek(self.data_base + (table << 12) + (block % 0xAA) * 0x18)
                if self.f.read(20) == digest:
                    break
            else:
                bad.append(block)
        return bad

    def next_block(self, block):
        hb = self.level0_hash_block(block)
        self.f.seek(self.data_base + (hb << 12) + (block % 0xAA) * 0x18)
        entry = self.f.read(0x18)
        return int.from_bytes(entry[0x15:0x18], "big")

    def blocks(self, start, count, consecutive):
        b = start
        for _ in range(count):
            yield b
            b = b + 1 if consecutive else self.next_block(b)

    def entries(self):
        table = b"".join(self.read_block(b) for b in self.blocks(self.ft_start, self.ft_blocks, False))
        out = []
        for i in range(0, len(table), 0x40):
            e = table[i:i + 0x40]
            name_len = e[0x28] & 0x3F
            if not name_len:
                out.append(None)          # keep slot numbering: parent fields index this table
                continue
            out.append({
                "name": e[:name_len].decode("latin-1"),
                "dir": bool(e[0x28] & 0x80),
                "consecutive": bool(e[0x28] & 0x40),
                "blocks": int.from_bytes(e[0x29:0x2C], "little"),
                "start": int.from_bytes(e[0x2F:0x32], "little"),
                "parent": struct.unpack_from(">h", e, 0x32)[0],
                "size": struct.unpack_from(">I", e, 0x34)[0],
            })
        return out

    def path_of(self, entries, idx):
        parts = []
        while idx >= 0:
            parts.append(entries[idx]["name"])
            idx = entries[idx]["parent"]
        return "/".join(reversed(parts))

    def extract(self, out_dir, list_only=False, tolerate_short=()):
        """tolerate_short: file names allowed to come out short (package cut off at the end)."""
        ents = self.entries()
        for i, e in enumerate(ents):
            if e is None:
                continue
            rel = self.path_of(ents, i)
            if list_only:
                print(f"{'<dir>' if e['dir'] else e['size']:>12}  {rel}")
                continue
            dest = os.path.join(out_dir, *rel.split("/"))
            if e["dir"]:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            remaining = e["size"]
            with open(dest, "wb") as w:
                for b in self.blocks(e["start"], e["blocks"], e["consecutive"]):
                    chunk = self.read_block(b)[:min(0x1000, remaining)]
                    w.write(chunk)
                    remaining -= len(chunk)
            if remaining and os.path.basename(rel) not in tolerate_short:
                raise IOError(f"{rel}: {remaining} bytes short")
        return ents


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pkg = STFS(sys.argv[1])
    print(f"{pkg.display_name}: header {pkg.header_size:#x}, shift {pkg.shift}, "
          f"file table {pkg.ft_blocks} blocks @ {pkg.ft_start}")
    pkg.extract(sys.argv[2], list_only="--list" in sys.argv)
