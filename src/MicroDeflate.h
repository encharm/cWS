#ifndef CWS_MICRODEFLATE_H
#define CWS_MICRODEFLATE_H

// Minimal raw-DEFLATE encoder for independent messages (permessage-deflate without
// context takeover, i.e. the shared-compressor mode). One fixed-Huffman block, greedy
// LZ77 over a 4-byte hash, no lazy matching, no stream state, and no per-message table
// clearing: stale entries are validated by tag, position and content. Output ends the way
// Z_SYNC_FLUSH ends (empty stored block); the returned length already excludes the
// trailing 00 00 ff ff, as RFC 7692 requires. Measured on a real BSON RPC stream: same
// ratio as zlib-ng level 1 at ~2x its speed, mostly by skipping the 64 KB hash reset
// zlib performs per message. Decodes with any inflater.
//
// Not for context takeover: matches never reference earlier messages.
//
// Hot-loop structure (measured on Zen 4, gcc -O3; the output is byte-identical to the
// previous layout, only the control flow changed):
//  - The hash table entry packs the 8-bit tag above a 24-bit position (+1), so tag, "older
//    than me" and "within 32 KB" are one subtraction and one compare with no second array.
//  - Literals are processed in pairs in an inner loop that the match path never enters:
//    one loop-bound check, one accumulator drain check and one taken branch per two
//    literals, and the candidate path is laid out as the unlikely branch.
//  - The first 32 bytes of a match are compared with SSE2 byte compares reduced to one
//    mismatch mask (portable 8-byte xor selects elsewhere), one branch for 90% of matches.
//  - The length symbol and the distance code table are precomputed per length / per code.

#include <cstdint>
#include <cstring>
#include <cstddef>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define CWS_MD_SSE2 1
#include <emmintrin.h>
#endif
#if defined(__GNUC__) || defined(__clang__)
#define CWS_MD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define CWS_MD_UNLIKELY(x) (x)
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
    // Entry: 8 more hash bits (the tag) in the top byte, position + 1 in the low 24 bits. A probe
    // for position i with tag t computes ((t << 24) | i) - entry, which is i - pos - 1 when the
    // tags agree and the entry is older; a differing tag or a newer (stale, from an earlier
    // message) position leaves a nonzero top byte, so "usable candidate" is d < 32768: one
    // compare, no second array, and the random load into the input only for candidates that
    // pass. Exact for positions below 2^23; see compress() for larger messages.
    uint32_t table[1 << HASH_BITS];
    static const uint32_t EMPTY = 0x00800000u; // tag 0, position 2^23: never in range below 2^23
    bool hugeDirty = false;

    static const uint16_t *lenBase() { static const uint16_t t[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258}; return t; }
    static const uint8_t *lenExtra() { static const uint8_t t[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0}; return t; }
    static const uint16_t *distBase() { static const uint16_t t[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577}; return t; }
    static const uint8_t *distExtra() { static const uint8_t t[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13}; return t; }

    struct Tables {
        uint16_t litCode[288]; uint8_t litBits[288];
        // Per match length: the complete length symbol (code + extra bits, at most 13 bits) and
        // its bit count, one load instead of lenCode -> litCode/litBits/lenBase/lenExtra.
        uint32_t lenSym[259]; uint8_t lenSymBits[259];
        // Per distance code: reversed 5-bit code | extra bits << 5 | base << 16.
        uint32_t distPacked[30];
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
            for (int c = 0; c < 29; c++) for (int l = lenBase()[c]; l < (c < 28 ? lenBase()[c + 1] : 259); l++) {
                lenSym[l] = litCode[257 + c] | ((uint32_t) (l - lenBase()[c]) << litBits[257 + c]);
                lenSymBits[l] = (uint8_t) (litBits[257 + c] + lenExtra()[c]);
            }
            for (int c = 0; c < 30; c++) distPacked[c] = rev(c, 5) | ((uint32_t) distExtra()[c] << 5) | ((uint32_t) distBase()[c] << 16);
        }
    };
    static const Tables &tables() { static const Tables t; return t; }

    // 64-bit accumulator drained 8 bytes at a time with one unaligned store (the output
    // buffer has slack for the over-write, see bound()). Literals (8 or 9 bits) drain the
    // accumulator conditionally, in the main loop once per pair (two appends add at most 18
    // bits to fewer than 32, so nothing overflows and the byte stream is unchanged): the
    // drain pattern repeats every few symbols and predicts well, and an unconditional store
    // per literal made incompressible input store-bound on Zen 4 (+58%). Match symbols vary
    // in size, so their drain branch mispredicted; they store unconditionally instead.
    struct BitWriter {
        uint8_t *p; uint64_t acc = 0; int n = 0;
        inline void put(uint32_t v, int bits) {
            acc |= (uint64_t) v << n; n += bits;
            memcpy(p, &acc, 8); p += n >> 3; acc >>= n & ~7; n &= 7;
        }
        inline void putLit(uint32_t v, int bits) {
            acc |= (uint64_t) v << n; n += bits;
            if (n >= 32) { memcpy(p, &acc, 8); p += n >> 3; acc >>= n & ~7; n &= 7; }
        }
        inline void putRaw(uint32_t v, int bits) { acc |= (uint64_t) v << n; n += bits; }
        inline void drain32() { if (n >= 32) { memcpy(p, &acc, 8); p += n >> 3; acc >>= n & ~7; n &= 7; } }
        inline void flushByte() { while (n > 0) { *p++ = (uint8_t) acc; acc >>= 8; n -= 8; } acc = 0; n = 0; }
    };

    static inline uint32_t load32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
    static inline uint64_t load64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
    static inline uint32_t hash32(const uint8_t *p) { return load32(p) * 2654435761u; }
    static inline uint32_t slot(uint32_t h) { return h >> (32 - HASH_BITS); }
    // The 8 hash bits below the slot bits, placed in the top byte of a table entry.
    static inline uint32_t tagBits(uint32_t h) { return (h << HASH_BITS) & 0xff000000u; }
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
    static inline int ctz32(uint32_t v) {
#ifdef _MSC_VER
        unsigned long r; _BitScanForward(&r, v); return (int) r;
#else
        return __builtin_ctz(v);
#endif
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
    // Same when at least 32 bytes are readable on both sides: the first 32 bytes are compared
    // without a data-dependent branch, so a match shorter than that (90% of them on the RPC
    // capture) costs one branch instead of a loop exit whose trip count varies per match.
    static inline size_t matchLength32(const uint8_t *a, const uint8_t *b, size_t max) {
#ifdef CWS_MD_SSE2
        __m128i a0 = _mm_loadu_si128((const __m128i *) a), b0 = _mm_loadu_si128((const __m128i *) b);
        __m128i a1 = _mm_loadu_si128((const __m128i *) (a + 16)), b1 = _mm_loadu_si128((const __m128i *) (b + 16));
        uint32_t ne = ~((uint32_t) _mm_movemask_epi8(_mm_cmpeq_epi8(a0, b0)) | ((uint32_t) _mm_movemask_epi8(_mm_cmpeq_epi8(a1, b1)) << 16));
        if (ne) return (size_t) ctz32(ne);
#else
        uint64_t x0 = load64(a) ^ load64(b), x1 = load64(a + 8) ^ load64(b + 8);
        uint64_t x2 = load64(a + 16) ^ load64(b + 16), x3 = load64(a + 24) ^ load64(b + 24);
        uint64_t z0 = (uint64_t) 0 - (x0 == 0), z1 = (uint64_t) 0 - (x1 == 0), z2 = (uint64_t) 0 - (x2 == 0);
        uint64_t x = x0 | (x1 & z0) | (x2 & z0 & z1) | (x3 & z0 & z1 & z2);
        size_t base = (8 & z0) + (8 & z0 & z1) + (8 & z0 & z1 & z2);
        if (x) return base + (ctz64(x) >> 3);
#endif
        return 32 + matchLength(a + 32, b + 32, max - 32);
    }

    void clearTable() { for (size_t k = 0; k < (size_t) 1 << HASH_BITS; k++) table[k] = EMPTY; }

public:
    Encoder() { clearTable(); tables(); }

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

    // Emits the symbols for in[start, length) into `w` (no block header or tail), matching
    // against everything before each position that the table knows about: within this span
    // or, for a window encoder, the history in front of it. Positions are absolute in `in`;
    // BITS is the table's index width. p = i + 1 throughout: entries hold position + 1, so
    // the probe key (tag | i) minus the entry is exactly i - pos - 1 and the range test needs
    // no adjustment.
    template <int BITS>
    static void encodeSpan(uint32_t *table, const uint8_t *in, size_t start, size_t length, BitWriter &w, uint32_t maxDist = 32768u) {
        const Tables &t = tables();
        const size_t limit = length >= 4 ? length - 4 : 0;
        size_t p = start + 1; const size_t plimit = limit + 1;
        size_t i;
        while (p < plimit) {
            uint32_t cand, d;
            for (;;) {                                     // literals, two per iteration
                if (p + 1 >= plimit) {                     // last position: one at a time
                    for (;;) {
                        if (p >= plimit) goto done;
                        uint32_t h = hash32(in + p - 1), s = (h >> (32 - BITS)), tb = tagBits(h);
                        d = (tb + (uint32_t) p - 1u) - table[s];
                        table[s] = tb | (uint32_t) p;
                        cand = (uint32_t) p - 2u - d;
                        if (d < maxDist && load32(in + cand) == load32(in + p - 1)) break;
                        w.putLit(t.litCode[in[p - 1]], t.litBits[in[p - 1]]); ++p;
                    }
                    break;
                }
                {
                    uint32_t h = hash32(in + p - 1), s = (h >> (32 - BITS)), tb = tagBits(h);
                    d = (tb + (uint32_t) p - 1u) - table[s];
                    table[s] = tb | (uint32_t) p;
                    cand = (uint32_t) p - 2u - d;
                    if (CWS_MD_UNLIKELY(d < maxDist)) { if (load32(in + cand) == load32(in + p - 1)) break; }
                }
                w.putRaw(t.litCode[in[p - 1]], t.litBits[in[p - 1]]);
                {
                    uint32_t h = hash32(in + p), s = (h >> (32 - BITS)), tb = tagBits(h);
                    d = (tb + (uint32_t) p) - table[s];
                    table[s] = tb | (uint32_t) (p + 1);
                    cand = (uint32_t) p - 1u - d;
                    if (CWS_MD_UNLIKELY(d < maxDist)) { if (load32(in + cand) == load32(in + p)) { ++p; break; } }
                }
                w.putRaw(t.litCode[in[p]], t.litBits[in[p]]);
                w.drain32();
                p += 2;
            }
            i = p - 1;                                     // match at i against cand, distance d + 1
            size_t maxLen = length - i; if (maxLen > 258) maxLen = 258;
            size_t m = 4 + (maxLen >= 36 ? matchLength32(in + cand + 4, in + i + 4, maxLen - 4) : matchLength(in + cand + 4, in + i + 4, maxLen - 4));
            uint32_t dist = d + 1u;
            w.put(t.lenSym[m], t.lenSymBits[m]);
            uint32_t dp = t.distPacked[distCode(dist)];    // code + its extra bits in one put (5 + 13)
            w.put((dp & 31) | ((dist - (dp >> 16)) << 5), 5 + ((dp >> 5) & 15));
            // Insert i + 1 as well (i + 1 <= limit, so its 4 bytes are readable; position `limit`
            // itself is harmless because nothing after it is hashed in this message).
            { uint32_t h1 = hash32(in + i + 1); table[(h1 >> (32 - BITS))] = tagBits(h1) | (uint32_t) (i + 2); }
            p = i + m + 1;
        }
    done:
        i = p - 1;
        if (i < start) i = start;
        for (; i < length; i++) w.putLit(t.litCode[in[i]], t.litBits[in[i]]);
    }

    // End of block and the sync-flush tail; falls back to stored blocks when the fixed tree
    // would expand the input. Returns the length without the tail.
    static size_t finish(BitWriter &w, const uint8_t *in, size_t length, uint8_t *out) {
        const Tables &t = tables();
        w.putLit(t.litCode[256], t.litBits[256]);          // end of block
        w.putLit(0, 1); w.putLit(0, 2); w.flushByte();     // empty stored block: BFINAL=0, BTYPE=00, then byte-align
        size_t pos = (size_t) (w.p - out);
        if (pos > length + 5 * (length / 65535 + 1) + 1) {
            return storeBlocks(in, length, out); // incompressible: stored blocks are smaller
        }
        out[pos++] = 0; out[pos++] = 0; out[pos++] = 0xff; out[pos++] = 0xff;
        return pos - 4;
    }

    // `out` must have room for bound(length) bytes. Returns the compressed length
    // (without the 4-byte sync-flush tail).
    size_t compress(const uint8_t *in, size_t length, uint8_t *out) {
        BitWriter w{out};
        w.put(0, 1); w.put(1, 2);                          // BFINAL=0, BTYPE=01 (fixed Huffman)
        // Positions are stored in 24 bits. A message of 8 MB or more can leave entries whose
        // position reaches the tag byte or collides with EMPTY, so the table is cleared before
        // and after it (32 KB against 8 MB of input). Inside it a false positive only costs the
        // content compare: the distance is in range, and the position is in bounds because the
        // table only holds smaller positions of this message.
        if (length >= ((size_t) 1 << 23) || hugeDirty) { clearTable(); hugeDirty = length >= ((size_t) 1 << 23); }
        encodeSpan<HASH_BITS>(table, in, 0, length, w);
        return finish(w, in, length, out);
    }

    friend class Window;
};

// Per-connection encoder with context takeover (RFC 7692 without server_no_context_takeover):
// messages are appended to a buffer that keeps the last 32 KB sent on the connection, and the
// hash table persists across messages, so a message may reference the ones before it. Same
// symbols, tables and hot loop as Encoder; only the history differs. 64 KB per connection:
// a 48 KB buffer (32 KB history + 16 KB append margin) and a 4096-entry table. When the
// margin is full the last 32 KB move to the front and the table entries are rebased.
// Measured on the RPC capture with zlib's fixed-tree encoder: 2.92x independent -> 3.82x with
// takeover; a 4 KB history would give only 3.04x, so the history is not configurable below 32 KB.
class Window {
    static const int TABLE_BITS = 12;
    static const size_t MAX_HISTORY = 32768, MARGIN = 16384, BUF = MAX_HISTORY + MARGIN;
    uint32_t table[1 << TABLE_BITS];
    uint8_t buf[BUF];
    size_t end = 0;          // bytes in buf; the next message is appended here
    // The negotiated window (1 << windowBits, at most 32 KB): both how much history is kept
    // and the farthest distance a match may reach, since that is all the client's inflater
    // holds when a smaller server_max_window_bits was advertised.
    size_t history;

    void clear() { for (size_t k = 0; k < (size_t) 1 << TABLE_BITS; k++) table[k] = Encoder::EMPTY; end = 0; }

    // Keep the last HISTORY bytes, drop older table entries, shift the rest.
    void slide() {
        size_t keep = end < history ? end : history, shift = end - keep;
        if (shift == 0) return;
        memmove(buf, buf + shift, keep);
        for (size_t k = 0; k < (size_t) 1 << TABLE_BITS; k++) {
            uint32_t e = table[k], pos1 = e & 0x00ffffffu;
            table[k] = (e != Encoder::EMPTY && pos1 > shift) ? (e - (uint32_t) shift) : Encoder::EMPTY;
        }
        end = keep;
    }

    // The last three positions of a message are never hashed while it is encoded (no 4 bytes
    // yet); once the next message follows they can be.
    void hashTail(size_t start) {
        for (size_t q = start >= 3 ? start - 3 : 0; q < start && q + 4 <= end; q++) {
            uint32_t h = Encoder::hash32(buf + q);
            table[h >> (32 - TABLE_BITS)] = Encoder::tagBits(h) | (uint32_t) (q + 1);
        }
    }

public:
    explicit Window(int windowBits = 15) : history((size_t) 1 << (windowBits < 8 ? 8 : windowBits > 15 ? 15 : windowBits)) { clear(); Encoder::tables(); }
    void reset() { clear(); }

    // Adds bytes that went out on this connection without being compressed here (an
    // independently compressed prepared message, spliced in as-is) so the history stays in
    // step with the client's inflater. Not indexed: later messages will not reference them,
    // which is the right trade for fan-out payloads (the next push is itself independent).
    void append(const uint8_t *in, size_t length) {
        size_t done = 0;
        while (done < length) {
            if (end + (length - done) > BUF) {
                slide();
            }
            size_t n = length - done; if (n > BUF - end) n = BUF - end;
            memcpy(buf + end, in + done, n);
            end += n; done += n;
        }
    }

    // `out` must have room for bound(length) bytes. Returns the compressed length (without
    // the 4-byte sync-flush tail). The message becomes part of the history.
    size_t compress(const uint8_t *in, size_t length, uint8_t *out) {
        Encoder::BitWriter w{out};
        w.put(0, 1); w.put(1, 2);                          // BFINAL=0, BTYPE=01 (fixed Huffman)
        size_t done = 0;
        while (done < length) {
            if (end + (length - done) > BUF) {
                slide();
            }
            size_t n = length - done; if (n > BUF - end) n = BUF - end;
            memcpy(buf + end, in + done, n);
            size_t start = end; end += n;
            hashTail(start);
            Encoder::encodeSpan<TABLE_BITS>(table, buf, start, end, w, (uint32_t) history);
            done += n;
        }
        // A stored fallback still puts every byte of the message on the wire, so the
        // client's window matches ours either way.
        return Encoder::finish(w, in, length, out);
    }
};

}
}

#endif // CWS_MICRODEFLATE_H
