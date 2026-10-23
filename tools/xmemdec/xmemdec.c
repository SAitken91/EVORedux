/* Fast XMemCompress (LZX, 128 KiB window) decoder for Trials Evolution Xbox 360 DLC files.
 * Same algorithm as tools/xmem.py (which is verified against PC files), in C for speed.
 *
 *   int xmem_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
 * `in` starts at the XMem payload: u32 LE usize, u32 LE psize, frames. Returns 0 on success.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define WINDOW_BITS 17
#define WINDOW_SIZE (1u << WINDOW_BITS)
#define FRAME_SIZE 32768
#define NUM_CHARS 256
#define NUM_SLOTS 34
#define MAIN_SYMS (NUM_CHARS + NUM_SLOTS * 8)
#define LEN_SYMS 249
#define MAX_BITS 16

typedef struct { const uint8_t *p, *end; uint32_t buf; int left; } Bits;

static void ensure(Bits *b, int n) {
    while (b->left < n) {
        uint32_t w = 0;
        if (b->p + 1 < b->end) w = b->p[0] | (b->p[1] << 8);
        else if (b->p < b->end) w = b->p[0];
        b->p += 2;
        b->buf |= w << (16 - b->left);
        b->left += 16;
    }
}
static uint32_t peek(Bits *b, int n) { ensure(b, n); return b->buf >> (32 - n); }
static void drop(Bits *b, int n) { b->buf <<= n; b->left -= n; }
static uint32_t rd(Bits *b, int n) {
    if (n == 0) return 0;
    if (n > 16) { uint32_t hi = rd(b, n - 16); return (hi << 16) | rd(b, 16); }
    uint32_t v = peek(b, n); drop(b, n); return v;
}

typedef struct { uint16_t count[MAX_BITS + 1]; uint16_t sym[MAIN_SYMS]; } Huff;

static int build(Huff *h, const uint8_t *lens, int n) {
    uint16_t offs[MAX_BITS + 2];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int l = 1; l <= MAX_BITS; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}
static int decode(Bits *b, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAX_BITS; len++) {
        code |= (int)rd(b, 1);
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

typedef struct {
    uint8_t *window; uint32_t wpos;
    uint8_t main_len[MAIN_SYMS], len_len[LEN_SYMS];
    Huff main, lengths, aligned;
    int has_lengths;
    uint32_t R[3];
    int header_read, block_type;
    uint32_t block_remaining;
    uint8_t extra[51]; uint32_t base[51];
    Bits bits;
} LZX;

static void lzx_init(LZX *z) {
    memset(z, 0, sizeof *z);
    z->window = (uint8_t *)calloc(WINDOW_SIZE, 1);
    z->R[0] = z->R[1] = z->R[2] = 1;
    for (int i = 0; i < 51; i++) z->extra[i] = (uint8_t)(i < 2 ? 0 : ((i - 2) / 2 < 17 ? (i - 2) / 2 : 17));
    z->base[0] = 0;
    for (int i = 1; i < 51; i++) z->base[i] = z->base[i - 1] + (1u << z->extra[i - 1]);
}

static int read_lengths(LZX *z, uint8_t *lens, int first, int last) {
    uint8_t pre_len[20]; Huff pre;
    for (int i = 0; i < 20; i++) pre_len[i] = (uint8_t)rd(&z->bits, 4);
    build(&pre, pre_len, 20);
    int x = first;
    while (x < last) {
        int s = decode(&z->bits, &pre);
        if (s < 0) return -1;
        if (s == 17) { int n = rd(&z->bits, 4) + 4; for (int i = 0; i < n && x < last; i++) lens[x++] = 0; }
        else if (s == 18) { int n = rd(&z->bits, 5) + 20; for (int i = 0; i < n && x < last; i++) lens[x++] = 0; }
        else if (s == 19) {
            int n = rd(&z->bits, 1) + 4;
            int t = decode(&z->bits, &pre);
            if (t < 0) return -1;
            int v = ((int)lens[x] - t) % 17; if (v < 0) v += 17;
            for (int i = 0; i < n && x < last; i++) lens[x++] = (uint8_t)v;
        } else {
            int v = ((int)lens[x] - s) % 17; if (v < 0) v += 17;
            lens[x++] = (uint8_t)v;
        }
    }
    return 0;
}

static void put(LZX *z, uint8_t c, uint8_t *out, uint32_t *n) {
    z->window[z->wpos] = c; z->wpos = (z->wpos + 1) & (WINDOW_SIZE - 1);
    out[(*n)++] = c;
}

/* Decodes one XMem chunk into out[*pos .. frame_end). A match may run past frame_end (the frames
 * are pieces of one continuous output); the overrun counts toward the next frame. */
static int decode_frame(LZX *z, const uint8_t *chunk, size_t clen, uint8_t *out, uint32_t *pos,
                        uint32_t frame_end, uint32_t total) {
    Bits *b = &z->bits;
    b->p = chunk; b->end = chunk + clen; b->buf = 0; b->left = 0;
    if (!z->header_read) { if (rd(b, 1)) { rd(b, 16); rd(b, 16); } z->header_read = 1; }
    uint32_t n = *pos;
    while (n < frame_end) {
        if (z->block_remaining == 0) {
            z->block_type = (int)rd(b, 3);
            uint32_t hi = rd(b, 16), lo = rd(b, 8);
            z->block_remaining = (hi << 8) | lo;
            if (z->block_type == 2) {
                uint8_t al[8]; for (int i = 0; i < 8; i++) al[i] = (uint8_t)rd(b, 3);
                build(&z->aligned, al, 8);
            }
            if (z->block_type == 1 || z->block_type == 2) {
                if (read_lengths(z, z->main_len, 0, 256) || read_lengths(z, z->main_len, 256, MAIN_SYMS)) return -2;
                build(&z->main, z->main_len, MAIN_SYMS);
                if (read_lengths(z, z->len_len, 0, LEN_SYMS)) return -2;
                z->has_lengths = 0;
                for (int i = 0; i < LEN_SYMS; i++) if (z->len_len[i]) z->has_lengths = 1;
                build(&z->lengths, z->len_len, LEN_SYMS);
            } else if (z->block_type == 3) {
                if (b->left == 0) ensure(b, 16);
                if (b->left & 15) drop(b, b->left & 15);
                if (b->left > 16) b->p -= 2;
                b->left = 0; b->buf = 0;
                if (b->p + 12 > b->end) return -3;
                memcpy(z->R, b->p, 12); b->p += 12;
            } else return -4;
        }
        uint32_t take = z->block_remaining < frame_end - n ? z->block_remaining : frame_end - n;
        if (z->block_type == 3) {
            if (b->p + take > b->end) return -5;
            for (uint32_t i = 0; i < take; i++) put(z, *b->p++, out, &n);
            z->block_remaining -= take;
            if (z->block_remaining == 0 && (take & 1)) b->p++;
            continue;
        }
        uint32_t end = n + take;
        while (n < end) {
            int s = decode(b, &z->main);
            if (s < 0) return -6;
            if (s < NUM_CHARS) { put(z, (uint8_t)s, out, &n); continue; }
            s -= NUM_CHARS;
            uint32_t len = s & 7;
            if (len == 7) { int e = z->has_lengths ? decode(b, &z->lengths) : -1; if (e < 0) return -7; len += (uint32_t)e; }
            len += 2;
            int slot = s >> 3;
            uint32_t off;
            if (slot > 2) {
                int ex = z->extra[slot];
                off = z->base[slot] - 2;
                if (z->block_type == 2 && ex >= 3) {
                    if (ex > 3) off += rd(b, ex - 3) << 3;
                    int a = decode(b, &z->aligned); if (a < 0) return -8;
                    off += (uint32_t)a;
                } else off += rd(b, ex);
                z->R[2] = z->R[1]; z->R[1] = z->R[0]; z->R[0] = off;
            } else if (slot == 0) off = z->R[0];
            else if (slot == 1) { off = z->R[1]; z->R[1] = z->R[0]; z->R[0] = off; }
            else { off = z->R[2]; z->R[2] = z->R[0]; z->R[0] = off; }
            uint32_t src = (z->wpos - off) & (WINDOW_SIZE - 1);
            for (uint32_t i = 0; i < len; i++) {
                if (n >= total) return -9;
                put(z, z->window[src], out, &n);
                src = (src + 1) & (WINDOW_SIZE - 1);
            }
        }
        z->block_remaining -= take;
        if (n > end) z->block_remaining -= n - end;
    }
    *pos = n;
    return 0;
}

__declspec(dllexport) int xmem_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    if (in_len < 8) return -1;
    uint32_t usize, psize;
    memcpy(&usize, in, 4); memcpy(&psize, in + 4, 4);
    if (usize > out_len || 8 + (size_t)psize > in_len) return -1;
    size_t o = 8, end = 8 + psize;
    uint32_t done = 0, frame_end = 0;
    LZX *z = (LZX *)malloc(sizeof(LZX));
    lzx_init(z);
    int rc = 0;
    while (o < end && done < usize) {
        uint32_t frame = usize - frame_end < FRAME_SIZE ? usize - frame_end : FRAME_SIZE;
        if (in[o] == 0xFF) { frame = (in[o + 1] << 8) | in[o + 2]; o += 3; }
        uint32_t csize = (in[o] << 8) | in[o + 1]; o += 2;
        if (o + csize > end) { rc = -10; break; }
        frame_end += frame;
        if (frame_end > usize) frame_end = usize;
        if (done < frame_end) {
            rc = decode_frame(z, in + o, csize, out, &done, frame_end, usize);
            if (rc) break;
        }
        o += csize;
    }
    free(z->window); free(z);
    return rc ? rc : (done == usize ? 0 : -11);
}
