"""Xbox 360 XMemCompress (LZX) decoder, for Trials Evolution DLC files.

Trials DLC pak entries are deflated like PC ones, but most Xbox files are additionally XMem-packed:
    u32 LE uncompressed size, u32 LE packed size, then XMem frames:
        [0xFF, u16 BE frame size]  (only when the frame isn't the full 32 KiB)
        u16 BE compressed chunk size, chunk bytes (LZX bitstream, continues across frames)
The LZX part follows the standard decoder (libmspack lzxd), window 128 KiB.

Usage: python tools/xmem.py <in> <out>
"""
import struct
import sys

MIN_MATCH = 2
NUM_CHARS = 256
FRAME = 32768


class Bits:
    def __init__(self):
        self.data = b""
        self.pos = 0
        self.buf = 0
        self.left = 0

    def feed(self, chunk):
        self.data, self.pos = chunk, 0

    def _word(self):
        if self.pos + 1 < len(self.data):
            w = self.data[self.pos] | (self.data[self.pos + 1] << 8)
        elif self.pos < len(self.data):
            w = self.data[self.pos]
        else:
            w = 0
        self.pos += 2
        return w

    def ensure(self, n):
        while self.left < n:
            self.buf = (self.buf << 16) | self._word()
            self.left += 16

    def peek(self, n):
        self.ensure(n)
        return (self.buf >> (self.left - n)) & ((1 << n) - 1)

    def read(self, n):
        if n == 0:
            return 0
        v = self.peek(n)
        self.left -= n
        self.buf &= (1 << self.left) - 1 if self.left else 0
        return v

    def align(self):
        drop = self.left % 16
        self.read(drop) if drop else None

    def raw_byte(self):
        # only valid when no bits are buffered (after align with left == 0)
        b = self.data[self.pos]
        self.pos += 1
        return b


class Huffman:
    def __init__(self, lengths):
        self.lookup = {}
        self.max = max(lengths) if lengths else 0
        code = 0
        for length in range(1, self.max + 1):
            for sym, l in enumerate(lengths):
                if l == length:
                    self.lookup[(length, code)] = sym
                    code += 1
            code <<= 1

    def decode(self, bits):
        code = 0
        for length in range(1, self.max + 1):
            code = (code << 1) | bits.read(1)
            sym = self.lookup.get((length, code))
            if sym is not None:
                return sym
        raise ValueError("bad huffman code")


def lzx_tables(window_bits):
    extra, base = [], []
    e, b = 0, 0
    for i in range(51):
        extra.append(e)
        base.append(b)
        if i >= 2 and i % 2 == 1 and e < 17:
            pass
        b += 1 << e
        if i >= 3 and i % 2 == 1 and e < 17:
            e += 1
    # standard: extra_bits = 0,0,0,0,1,1,2,2,...,17,17,...
    extra = [0, 0] + [min(17, (i - 2) // 2) for i in range(2, 51)]
    base = [0]
    for i in range(1, 51):
        base.append(base[-1] + (1 << extra[i - 1]))
    slots = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}[window_bits]
    return extra, base, slots


class LZX:
    def __init__(self, window_bits=17):
        self.window = bytearray(1 << window_bits)
        self.wpos = 0
        self.extra, self.base, self.slots = lzx_tables(window_bits)
        self.main_len = [0] * (NUM_CHARS + self.slots * 8)
        self.len_len = [0] * 249
        self.R = [1, 1, 1]
        self.bits = Bits()
        self.header_read = False
        self.block_remaining = 0
        self.block_type = 0

    def read_lengths(self, lens, first, last):
        pre = Huffman([self.bits.read(4) for _ in range(20)])
        x = first
        while x < last:
            z = pre.decode(self.bits)
            if z == 17:
                n = self.bits.read(4) + 4
                lens[x:x + n] = [0] * n
                x += n
            elif z == 18:
                n = self.bits.read(5) + 20
                lens[x:x + n] = [0] * n
                x += n
            elif z == 19:
                n = self.bits.read(1) + 4
                z = pre.decode(self.bits)
                z = (lens[x] - z) % 17
                lens[x:x + n] = [z] * n
                x += n
            else:
                lens[x] = (lens[x] - z) % 17
                x += 1

    def put(self, b):
        self.window[self.wpos] = b
        self.wpos = (self.wpos + 1) % len(self.window)

    def decode_frame(self, chunk, out_len):
        bits = self.bits
        bits.feed(chunk)
        if not self.header_read:
            if bits.read(1):
                bits.read(16), bits.read(16)          # intel e8 file size (unused)
            self.header_read = True
        out = bytearray()
        while len(out) < out_len:
            if self.block_remaining == 0:
                self.block_type = bits.read(3)
                self.block_remaining = (bits.read(16) << 8) | bits.read(8)
                if self.block_type == 2:
                    self.aligned = Huffman([bits.read(3) for _ in range(8)])
                if self.block_type in (1, 2):
                    self.read_lengths(self.main_len, 0, 256)
                    self.read_lengths(self.main_len, 256, len(self.main_len))
                    self.main = Huffman(self.main_len)
                    self.read_lengths(self.len_len, 0, 249)
                    self.lengths = Huffman(self.len_len) if any(self.len_len) else None
                elif self.block_type == 3:
                    bits.align() if bits.left % 16 else None
                    if bits.left == 0:
                        pass
                    # re-read: R0..R2 are raw little-endian u32s
                    if bits.left:                     # 16 buffered bits belong to the raw stream
                        bits.pos -= 2
                        bits.left, bits.buf = 0, 0
                    raw = bytes(bits.raw_byte() for _ in range(12))
                    self.R = list(struct.unpack("<III", raw))
                else:
                    raise ValueError(f"bad LZX block type {self.block_type}")
            take = min(self.block_remaining, out_len - len(out))
            if self.block_type == 3:
                for _ in range(take):
                    b = bits.raw_byte()
                    out.append(b)
                    self.put(b)
                self.block_remaining -= take
                if self.block_remaining == 0 and take % 2:
                    bits.raw_byte()                   # pad to 16 bits
                continue
            end = len(out) + take
            while len(out) < end:
                sym = self.main.decode(bits)
                if sym < NUM_CHARS:
                    out.append(sym)
                    self.put(sym)
                    continue
                sym -= NUM_CHARS
                length = sym & 7
                if length == 7:
                    length += self.lengths.decode(bits)
                length += MIN_MATCH
                slot = sym >> 3
                if slot > 2:
                    extra = self.extra[slot]
                    off = self.base[slot] - 2
                    if self.block_type == 2 and extra >= 3:
                        if extra > 3:
                            off += bits.read(extra - 3) << 3
                        off += self.aligned.decode(bits)
                    else:
                        off += bits.read(extra)
                    self.R = [off, self.R[0], self.R[1]]
                elif slot == 0:
                    off = self.R[0]
                elif slot == 1:
                    off = self.R[1]
                    self.R[1], self.R[0] = self.R[0], off
                else:
                    off = self.R[2]
                    self.R[2], self.R[0] = self.R[0], off
                src = (self.wpos - off) % len(self.window)
                for _ in range(length):
                    b = self.window[src]
                    out.append(b)
                    self.put(b)
                    src = (src + 1) % len(self.window)
            self.block_remaining -= take
            if len(out) > end:                       # match ran past the block end
                self.block_remaining -= len(out) - end
        bits.left, bits.buf = 0, 0                   # frames start word-aligned
        return bytes(out[:out_len])


def is_xmem(b):
    if len(b) < 11:
        return False
    usize, psize = struct.unpack_from("<II", b, 0)
    return psize + 8 == len(b) and usize >= psize // 2 and b[8] in (0xFF,) + tuple(range(0, 0x81))


def _load_native():
    """tools/xmemdec/xmemdec.dll (built by its build.bat) is ~1000x faster than the Python decoder."""
    try:
        import ctypes
        import os
        lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "xmemdec", "xmemdec.dll"))
        lib.xmem_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        lib.xmem_decompress.restype = ctypes.c_int
        return lib, ctypes
    except OSError:
        return None, None


_NATIVE, _ctypes = _load_native()


def xmem_decompress(b, partial_ok=False):
    """partial_ok: on a decode error, return what was decoded with the rest zero-filled."""
    if _NATIVE is not None:
        usize = struct.unpack_from("<I", b, 0)[0]
        out = _ctypes.create_string_buffer(usize)
        rc = _NATIVE.xmem_decompress(bytes(b), len(b), out, usize)
        if rc != 0 and not partial_ok:
            raise ValueError(f"xmem decode failed ({rc})")
        return out.raw
    return xmem_decompress_py(b)


def xmem_decompress_py(b):
    usize, psize = struct.unpack_from("<II", b, 0)
    o, end = 8, 8 + psize
    lzx = LZX(17)
    out = bytearray()
    while o < end and len(out) < usize:
        frame = min(FRAME, usize - len(out))
        if b[o] == 0xFF:
            frame = struct.unpack_from(">H", b, o + 1)[0]
            o += 3
        csize = struct.unpack_from(">H", b, o)[0]
        o += 2
        out += lzx.decode_frame(b[o:o + csize], frame)
        o += csize
    return bytes(out[:usize])


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    open(sys.argv[2], "wb").write(xmem_decompress(data))
