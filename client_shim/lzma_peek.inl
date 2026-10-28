// Minimal LZMA ("LZMA alone", as in track.trk) decoder that stops after `out_size` bytes.
// Used to read the first chunks of a track body (MET2, DLC0) without decoding the whole track.
// Plain C++ with no game dependencies so it can be tested offline (tools: x360_dlc_objects).
#pragma once
#include <string.h>

namespace lzma_peek {

struct RangeDecoder {
    const unsigned char* in;
    size_t size, pos;
    unsigned range, code;
    bool bad;
    unsigned char byte() {
        if (pos < size) return in[pos++];
        bad = true;
        return 0;
    }
    void init() {
        range = 0xffffffffu; code = 0; bad = false;
        byte();
        for (int i = 0; i < 4; i++) code = (code << 8) | byte();
    }
    void normalize() {
        if (range < (1u << 24)) { range <<= 8; code = (code << 8) | byte(); }
    }
    int bit(unsigned short* p) {
        unsigned bound = (range >> 11) * *p;
        int b;
        if (code < bound) { range = bound; *p += (unsigned short)((2048 - *p) >> 5); b = 0; }
        else { range -= bound; code -= bound; *p -= (unsigned short)(*p >> 5); b = 1; }
        normalize();
        return b;
    }
    unsigned direct(int count) {
        unsigned r = 0;
        while (count--) {
            range >>= 1;
            unsigned t = (code - range) >> 31;   // 1 when code < range
            code -= range & (t - 1);
            r = (r << 1) | (1 - t);
            normalize();
        }
        return r;
    }
};

static unsigned tree(RangeDecoder& rc, unsigned short* probs, int bits) {
    unsigned m = 1;
    for (int i = 0; i < bits; i++) m = (m << 1) | rc.bit(probs + m);
    return m - (1u << bits);
}

static unsigned reverse_tree(RangeDecoder& rc, unsigned short* probs, int bits) {
    unsigned m = 1, sym = 0;
    for (int i = 0; i < bits; i++) {
        int b = rc.bit(probs + m);
        m = (m << 1) | b;
        sym |= (unsigned)b << i;
    }
    return sym;
}

struct LenDecoder {
    unsigned short choice, choice2, low[16][8], mid[16][8], high[256];
    unsigned decode(RangeDecoder& rc, unsigned pos_state) {
        if (!rc.bit(&choice)) return tree(rc, low[pos_state], 3);
        if (!rc.bit(&choice2)) return 8 + tree(rc, mid[pos_state], 3);
        return 16 + tree(rc, high, 8);
    }
};

// props: the 5 header bytes (lc/lp/pb byte + dictionary size); data: the range-coded stream.
// Decodes up to out_size bytes into out; returns the number produced (stops early on bad input).
// lc + lp must be <= 4 (the track files use lc 3, lp 0), otherwise 0 is returned.
inline size_t decode(const unsigned char* props, const unsigned char* data, size_t data_size,
                     unsigned char* out, size_t out_size) {
    unsigned d = props[0];
    if (d >= 9 * 5 * 5) return 0;
    unsigned lc = d % 9, lp = (d / 9) % 5, pb = d / 45;
    if (lc + lp > 4) return 0;
    static const size_t LIT = 0x300u << 4;
    struct Probs {
        unsigned short is_match[12][16], is_rep[12], is_rep0[12], is_rep1[12], is_rep2[12], is_rep0_long[12][16];
        unsigned short pos_slot[4][64], pos_special[115], align[16];
        LenDecoder len, rep_len;
        unsigned short literal[0x300u << 4];
    };
    Probs* p = new Probs;
    unsigned short* all = (unsigned short*)p;
    for (size_t i = 0; i < sizeof(Probs) / 2; i++) all[i] = 1024;
    (void)LIT;

    RangeDecoder rc{data, data_size, 0};
    rc.init();
    size_t n = 0;
    unsigned state = 0, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    const unsigned pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
    while (n < out_size && !rc.bad) {
        unsigned pos_state = (unsigned)n & pb_mask;
        if (!rc.bit(&p->is_match[state][pos_state])) {
            unsigned prev = n ? out[n - 1] : 0;
            unsigned short* probs = p->literal + 0x300u * ((((unsigned)n & lp_mask) << lc) + (prev >> (8 - lc)));
            unsigned sym = 1;
            if (state >= 7) {
                if (rep0 >= n) break;
                unsigned match = out[n - rep0 - 1];
                while (sym < 0x100) {
                    unsigned mbit = (match >> 7) & 1;
                    match <<= 1;
                    unsigned b = (unsigned)rc.bit(probs + ((1 + mbit) << 8) + sym);
                    sym = (sym << 1) | b;
                    if (mbit != b) break;
                }
            }
            while (sym < 0x100) sym = (sym << 1) | (unsigned)rc.bit(probs + sym);
            out[n++] = (unsigned char)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        unsigned len;
        if (rc.bit(&p->is_rep[state])) {
            if (!n) break;
            if (!rc.bit(&p->is_rep0[state])) {
                if (!rc.bit(&p->is_rep0_long[state][pos_state])) {
                    state = state < 7 ? 9 : 11;
                    if (rep0 >= n) break;
                    out[n] = out[n - rep0 - 1];
                    n++;
                    continue;
                }
            } else {
                unsigned dist;
                if (!rc.bit(&p->is_rep1[state])) dist = rep1;
                else {
                    if (!rc.bit(&p->is_rep2[state])) dist = rep2;
                    else { dist = rep3; rep3 = rep2; }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = p->rep_len.decode(rc, pos_state);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2; rep2 = rep1; rep1 = rep0;
            len = p->len.decode(rc, pos_state);
            state = state < 7 ? 7 : 10;
            unsigned slot = tree(rc, p->pos_slot[len < 4 ? len : 3], 6);
            if (slot < 4) rep0 = slot;
            else {
                unsigned bits = (slot >> 1) - 1;
                rep0 = (2 | (slot & 1)) << bits;
                if (slot < 14) rep0 += reverse_tree(rc, p->pos_special + rep0 - slot, bits);
                else {
                    rep0 += rc.direct(bits - 4) << 4;
                    rep0 += reverse_tree(rc, p->align, 4);
                }
                if (rep0 == 0xffffffffu) break;   // end marker
            }
        }
        len += 2;
        if (rep0 >= n) break;
        while (len-- && n < out_size) { out[n] = out[n - rep0 - 1]; n++; }
    }
    delete p;
    return n;
}

}  // namespace lzma_peek
