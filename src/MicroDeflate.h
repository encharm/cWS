#ifndef CWS_MICRODEFLATE_H
#define CWS_MICRODEFLATE_H

// Minimal raw-DEFLATE encoder for independent messages (permessage-deflate without
// context takeover, i.e. the shared-compressor mode). One fixed-Huffman block, greedy
// LZ77 over a 4-byte hash, no lazy matching, no stream state, and no per-message table
// clearing: stale entries are validated by position and content. Output ends the way
// Z_SYNC_FLUSH ends (empty stored block); the returned length already excludes the
// trailing 00 00 ff ff, as RFC 7692 requires. Measured on a real BSON RPC stream: same
// ratio as zlib-ng level 1 at ~1.7x its speed, mostly by skipping the 64 KB hash reset
// zlib performs per message. Decodes with any inflater.
//
// Not for context takeover: matches never reference earlier messages.

#include <cstdint>
#include <cstring>
#include <cstddef>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace cWS {
namespace microdeflate {

// Worst case output for `length` input bytes (all literals at 9 bits + block overhead),
// plus slack for the 8-byte stores of the bit writer.
inline size_t bound(size_t length) {
    return length + length / 8 + 32;
}

class Encoder {
    static const int HASH_BITS = 13;
    uint32_t table[1 << HASH_BITS];
    // 8 more hash bits per slot: a candidate whose tag differs cannot match, and rejecting it
    // here saves the dependent random load into the input that dominated the literal path.
    uint8_t tags[1 << HASH_BITS];

    static const uint16_t *lenBase() { static const uint16_t t[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258}; return t; }
    static const uint8_t *lenExtra() { static const uint8_t t[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0}; return t; }
    static const uint16_t *distBase() { static const uint16_t t[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577}; return t; }
    static const uint8_t *distExtra() { static const uint8_t t[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13}; return t; }

    struct Tables {
        uint16_t litCode[288]; uint8_t litBits[288];
        uint8_t lenCode[259];
        uint8_t distCodeRev[30];
        static uint32_t rev(uint32_t v, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
        Tables() {
            for (int i = 0; i < 288; i++) {
                int bits, code;
                if (i < 144) { bits = 8; code = 0x30 + i; }
                else if (i < 256) { bits = 9; code = 0x190 + (i - 144); }
                else if (i < 280) { bits = 7; code = i - 256; }
                else { bits = 8; code = 0xC0 + (i - 280); }
                litCode[i] = (uint16_t) rev(code, bits); litBits[i] = (uint8_t) bits;
            }
            for (int c = 0; c < 29; c++) for (int l = lenBase()[c]; l < (c < 28 ? lenBase()[c + 1] : 259); l++) lenCode[l] = (uint8_t) c;
            for (int c = 0; c < 30; c++) distCodeRev[c] = (uint8_t) rev(c, 5);
        }
    };
    static const Tables &tables() { static const Tables t; return t; }

    // 64-bit accumulator flushed 8 bytes at a time with one unaligned store (the output
    // buffer has slack for the over-write, see bound()). Literals (8 or 9 bits) drain the
    // accumulator conditionally: their flush pattern repeats every few symbols and predicts
    // well, and an unconditional store per literal made incompressible input store-bound on
    // Zen 4 (+58%). Match symbols vary in size, so their flush branch mispredicted; they
    // store unconditionally instead (-7% on the RPC capture on Zen 4, -10% on Apple M).
    struct BitWriter {
        uint8_t *out; size_t pos = 0; uint64_t acc = 0; int n = 0;
        inline void put(uint32_t v, int bits) {
            acc |= (uint64_t) v << n; n += bits;
            memcpy(out + pos, &acc, 8); pos += n >> 3; acc >>= n & ~7; n &= 7;
        }
        inline void putLit(uint32_t v, int bits) {
            acc |= (uint64_t) v << n; n += bits;
            if (n >= 32) { memcpy(out + pos, &acc, 8); pos += n >> 3; acc >>= n & ~7; n &= 7; }
        }
        inline void flushByte() { while (n > 0) { out[pos++] = (uint8_t) acc; acc >>= 8; n -= 8; } acc = 0; n = 0; }
    };

    static inline uint32_t load32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
    static inline uint64_t load64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
    static inline uint32_t hash32(const uint8_t *p) { return load32(p) * 2654435761u; }
    static inline uint32_t slot(uint32_t h) { return h >> (32 - HASH_BITS); }
    static inline uint8_t tag(uint32_t h) { return (uint8_t) (h >> (32 - HASH_BITS - 8)); }
    // Distance code from the highest set bit of dist-1: codes 0-3 are exact, then two codes
    // per power of two, the lower one chosen by the bit below the msb. Replaces a 32 KB table.
    static inline int distCode(uint32_t dist) {
        uint32_t d = dist - 1;
        if (d < 4) return (int) d;
#ifdef _MSC_VER
        unsigned long msb; _BitScanReverse(&msb, d);
#else
        int msb = 31 - __builtin_clz(d);
#endif
        return ((int) msb << 1) | (int) ((d >> (msb - 1)) & 1);
    }
    static inline int ctz64(uint64_t v) {
#ifdef _MSC_VER
        unsigned long r; _BitScanForward64(&r, v); return (int) r;
#else
        return __builtin_ctzll(v);
#endif
    }

    // Length of the common prefix of a and b, at most `max`: 8 bytes per step while
    // both sides have 8 bytes left (little-endian: the first differing byte is the
    // lowest set byte of the xor), then byte by byte.
    static inline size_t matchLength(const uint8_t *a, const uint8_t *b, size_t max) {
        size_t m = 0;
        while (m + 8 <= max) {
            uint64_t x = load64(a + m) ^ load64(b + m);
            if (x) return m + (ctz64(x) >> 3);
            m += 8;
        }
        while (m < max && a[m] == b[m]) m++;
        return m;
    }

public:
    Encoder() { memset(table, 0xff, sizeof(table)); memset(tags, 0, sizeof(tags)); tables(); }

    // Stored blocks: what an incompressible message costs, 5 bytes per 65535 plus the tail.
    static size_t storeBlocks(const uint8_t *in, size_t length, uint8_t *out) {
        size_t pos = 0;
        for (size_t i = 0; i < length; ) {
            size_t n = length - i > 65535 ? 65535 : length - i;
            out[pos++] = 0; // BFINAL=0, BTYPE=00
            out[pos++] = (uint8_t) n; out[pos++] = (uint8_t) (n >> 8);
            out[pos++] = (uint8_t) ~n; out[pos++] = (uint8_t) (~n >> 8);
            memcpy(out + pos, in + i, n); pos += n; i += n;
        }
        out[pos++] = 0; // empty stored block header, byte aligned; the 00 00 ff ff tail is implied
        return pos;
    }

    // `out` must have room for bound(length) bytes. Returns the compressed length
    // (without the 4-byte sync-flush tail).
    size_t compress(const uint8_t *in, size_t length, uint8_t *out) {
        const Tables &t = tables();
        BitWriter w{out};
        w.put(0, 1); w.put(1, 2);                          // BFINAL=0, BTYPE=01 (fixed Huffman)
        size_t i = 0;
        const size_t limit = length >= 4 ? length - 4 : 0;
        while (i < limit) {
            uint32_t h = hash32(in + i), s = slot(h);
            uint8_t g = tag(h);
            uint32_t cand = table[s];
            bool tagged = tags[s] == g;
            table[s] = (uint32_t) i;
            tags[s] = g;
            if (tagged && cand < i && i - cand <= 32768 && load32(in + cand) == load32(in + i)) {
                size_t maxLen = length - i; if (maxLen > 258) maxLen = 258;
                size_t m = 4 + matchLength(in + cand + 4, in + i + 4, maxLen - 4);
                uint32_t dist = (uint32_t) (i - cand);
                int lc = t.lenCode[m];
                // length code + its extra bits in one put (at most 8 + 5 bits), same for distance (5 + 13)
                w.put(t.litCode[257 + lc] | ((uint32_t) (m - lenBase()[lc]) << t.litBits[257 + lc]), t.litBits[257 + lc] + lenExtra()[lc]);
                int dc = distCode(dist);
                w.put(t.distCodeRev[dc] | ((dist - distBase()[dc]) << 5), 5 + distExtra()[dc]);
                if (i + 1 < limit) { uint32_t h1 = hash32(in + i + 1); table[slot(h1)] = (uint32_t) (i + 1); tags[slot(h1)] = tag(h1); }
                i += m;
            } else {
                w.putLit(t.litCode[in[i]], t.litBits[in[i]]);
                i++;
            }
        }
        for (; i < length; i++) w.putLit(t.litCode[in[i]], t.litBits[in[i]]);
        w.putLit(t.litCode[256], t.litBits[256]);          // end of block
        w.putLit(0, 1); w.putLit(0, 2); w.flushByte();     // empty stored block: BFINAL=0, BTYPE=00, then byte-align
        if (w.pos > length + 5 * (length / 65535 + 1) + 1) {
            return storeBlocks(in, length, out); // incompressible: stored blocks are smaller
        }
        out[w.pos++] = 0; out[w.pos++] = 0; out[w.pos++] = 0xff; out[w.pos++] = 0xff;
        return w.pos - 4;
    }
};

}
}

#endif // CWS_MICRODEFLATE_H
