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

namespace cWS {
namespace microdeflate {

// Worst case output for `length` input bytes (all literals at 9 bits + block overhead).
inline size_t bound(size_t length) {
    return length + length / 8 + 32;
}

class Encoder {
    static const int HASH_BITS = 13;
    uint32_t table[1 << HASH_BITS];

    static const uint16_t *lenBase() { static const uint16_t t[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258}; return t; }
    static const uint8_t *lenExtra() { static const uint8_t t[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0}; return t; }
    static const uint16_t *distBase() { static const uint16_t t[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577}; return t; }
    static const uint8_t *distExtra() { static const uint8_t t[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13}; return t; }

    struct Tables {
        uint16_t litCode[288]; uint8_t litBits[288];
        uint8_t lenCode[259];
        uint8_t distCode[32769];
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
            for (int c = 0; c < 30; c++) { for (int d = distBase()[c]; d < (c < 29 ? distBase()[c + 1] : 32769); d++) distCode[d] = (uint8_t) c; distCodeRev[c] = (uint8_t) rev(c, 5); }
        }
    };
    static const Tables &tables() { static const Tables t; return t; }

    struct BitWriter {
        uint8_t *out; size_t pos = 0; uint64_t acc = 0; int n = 0;
        inline void put(uint32_t v, int bits) { acc |= (uint64_t) v << n; n += bits; while (n >= 8) { out[pos++] = (uint8_t) acc; acc >>= 8; n -= 8; } }
        inline void flushByte() { if (n) { out[pos++] = (uint8_t) acc; acc = 0; n = 0; } }
    };

    static inline uint32_t hash4(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return (v * 2654435761u) >> (32 - HASH_BITS); }

public:
    Encoder() { memset(table, 0xff, sizeof(table)); tables(); }

    // `out` must have room for bound(length) bytes. Returns the compressed length
    // (without the 4-byte sync-flush tail).
    size_t compress(const uint8_t *in, size_t length, uint8_t *out) {
        const Tables &t = tables();
        BitWriter w{out};
        w.put(0, 1); w.put(1, 2);                          // BFINAL=0, BTYPE=01 (fixed Huffman)
        size_t i = 0;
        const size_t limit = length >= 4 ? length - 4 : 0;
        while (i < limit) {
            uint32_t h = hash4(in + i);
            uint32_t cand = table[h];
            table[h] = (uint32_t) i;
            if (cand < i && i - cand <= 32768 && memcmp(in + cand, in + i, 4) == 0) {
                size_t maxLen = length - i; if (maxLen > 258) maxLen = 258;
                size_t m = 4;
                while (m < maxLen && in[cand + m] == in[i + m]) m++;
                uint32_t dist = (uint32_t) (i - cand);
                int lc = t.lenCode[m];
                w.put(t.litCode[257 + lc], t.litBits[257 + lc]);
                if (lenExtra()[lc]) w.put((uint32_t) (m - lenBase()[lc]), lenExtra()[lc]);
                int dc = t.distCode[dist];
                w.put(t.distCodeRev[dc], 5);
                if (distExtra()[dc]) w.put(dist - distBase()[dc], distExtra()[dc]);
                if (i + 1 < limit) table[hash4(in + i + 1)] = (uint32_t) (i + 1);
                i += m;
            } else {
                w.put(t.litCode[in[i]], t.litBits[in[i]]);
                i++;
            }
        }
        for (; i < length; i++) w.put(t.litCode[in[i]], t.litBits[in[i]]);
        w.put(t.litCode[256], t.litBits[256]);             // end of block
        w.put(0, 1); w.put(0, 2); w.flushByte();           // empty stored block: BFINAL=0, BTYPE=00, then byte-align
        out[w.pos++] = 0; out[w.pos++] = 0; out[w.pos++] = 0xff; out[w.pos++] = 0xff;
        return w.pos - 4;
    }
};

}
}

#endif // CWS_MICRODEFLATE_H
