// microinflate.h - self-contained raw DEFLATE decoder for small, independent messages.
//
// Contract: cWS::microinflate::inflate(in, inLen, out, outCap) decodes a raw DEFLATE stream as
// received over permessage-deflate without context takeover, with the sync-flush tail
// (00 00 ff ff) already stripped: the stream ends right after the byte-aligned header of an
// empty non-final stored block, so the last real block is usually non-final. Returns the
// number of bytes written, or (size_t)-1 on any error. Never reads outside [in, in + inLen)
// and never writes outside [out, out + outCap) (bytes past the returned length inside the
// buffer may be scribbled). No allocation; about 12 KB of stack; thread-safe.
//
// End-of-stream rules (they match what zlib produces with the tail re-appended):
//   - input exhausted at a block boundary (fewer than 3 bits left): success;
//   - a stored-block header after which no input byte remains (the stripped tail): success,
//     whatever its BFINAL bit says;
//   - a block that ends with BFINAL=1: success, anything after it is ignored (RFC 7692
//     section 7.2.3.4 senders; this is what cWS's zlib path does with Z_STREAM_END);
//   - input exhausted inside a block, or a stored block whose LEN runs past the input: error.
//
// Design:
//   - 64-bit bit buffer. The fast loop refills branch-free by ORing in a word loaded right
//     after the previous refill ("nbits |= 56": 56..63 valid bits afterwards; the bytes past
//     the counted bits are re-ORed identically next time), the input-range check being
//     hoisted into the loop condition; once fewer than 24 input bytes remain it continues
//     over a zero-padded copy of them. The checked refill used elsewhere pads with virtual
//     zero bytes as well. Virtual bytes are counted in `overread`, so consuming past the end
//     is detected at the next block boundary instead of being checked per symbol.
//   - Huffman tables: primary tables of LITLEN_BITS / DIST_BITS bits plus second-level
//     subtables for longer codes, canonical. The build avoids the two store-to-load
//     forwarding chains that dominate zlib's and libdeflate's builds on short messages: the
//     length histogram is accumulated while the code lengths are decoded (a repeat code is one
//     add), and instead of a counting sort the lengths array is scanned once per code length
//     with SSE2 pcmpeqb/pmovmskb (SWAR fallback), 64 bytes per step with an early exit, which
//     yields the symbols in canonical (length, symbol) order; codewords come from a static
//     bit-reversal table. The primary table is filled by doubling the region of the codes
//     placed so far for each length: one write per symbol plus 16-byte copies.
//   - 32-bit entries laid out for BMI1 bextr: [7:0] bits consumed by the entry (code + extra
//     bits; a subtable pointer consumes the primary bits, a subtable entry the rest), [15:8]
//     index width of the next lookup, [31:30] type (literal, length/distance, subtable
//     pointer, end: EOB or invalid), [29:16] value (literal byte; length base [24:16] + extra
//     bit count [28:25]; distance symbol; subtable offset). "nbits -= entry" subtracts the
//     whole entry; only the low 6 bits of nbits are ever used.
//   - Fast loop after libdeflate's: the entry for the current position is looked up before the
//     refill that follows it, every entry is consumed before being dispatched, and with BMI1
//     the next index is one bextr from the unshifted bits (the shift leaves the dependency
//     chain: 5-cycle load + 1 instead of load + shift + and) and BMI2 shrx consumes the bits
//     without the register copies a destructive shr costs. Chosen once at runtime with
//     __builtin_cpu_supports; the portable instantiation is the same code with shift+mask.
//     Four primary-table literals per refill, a second refill before the distance, the next
//     entry looked up before the match copy; matches are copied with two unconditional
//     16-byte stores (distance >= 16), four 8-byte stores (8..15) or an 8-byte stride copy
//     after 8 byte-wise stores (1..7), looping only for lengths above 32. A checked slow loop
//     handles the last bytes of the output buffer.
//   - Static tables for fixed-Huffman blocks built once (thread-safe function-local static).
//   - Code validation as zlib's inflate_table: over-subscribed sets are rejected, incomplete
//     sets only allowed as a single code of length 1 (litlen/dist) or no distance code at all.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define MI_SSE2 1
#endif
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define MI_X86_BMI_DISPATCH 1
#endif

namespace cWS {
namespace microinflate {
namespace detail {

#if defined(__GNUC__) || defined(__clang__)
#define MI_LIKELY(x) __builtin_expect(!!(x), 1)
#define MI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define MI_INLINE inline __attribute__((always_inline))
#define MI_NOINLINE __attribute__((noinline))
#else
#define MI_LIKELY(x) (x)
#define MI_UNLIKELY(x) (x)
#define MI_INLINE __forceinline
#define MI_NOINLINE __declspec(noinline)
#endif

constexpr size_t FAIL = (size_t) -1;

// Table entry layout (see the header comment)
constexpr uint32_t TYPE_LIT = 0u;
constexpr uint32_t TYPE_LEN = 1u << 30;                     // length code, or a distance entry
constexpr uint32_t TYPE_SUB = 2u << 30;
constexpr uint32_t TYPE_END = 3u << 30;                     // value 0: end of block, 1: invalid code
constexpr uint32_t EXCEPTIONAL = 1u << 31;                  // TYPE_SUB or TYPE_END
constexpr uint32_t VALUE_MASK = 0x3fff;

constexpr int LITLEN_BITS = 10;
constexpr int DIST_BITS = 8;
constexpr int PRECODE_BITS = 7;
// Primary table plus room for the second-level subtables. zlib's "enough" for (288, 10, 15)
// is about 1340 entries and for (32, 8, 15) about 150; the build bails out (invalid) if a
// code would ever need more than this, which a valid code never does.
constexpr unsigned LITLEN_TABLE = (1u << LITLEN_BITS) + 640;
constexpr unsigned DIST_TABLE = (1u << DIST_BITS) + 640;
constexpr unsigned PRECODE_TABLE = 1u << PRECODE_BITS;
constexpr unsigned MAX_LENS = 288 + 32;
constexpr unsigned LENS_SLACK = 64;                         // scan (64-byte steps) overshoot room after the lengths
constexpr unsigned REP_SLACK = 144;                         // a repeat may run past hlit + hdist before being rejected

// The fast loop needs this much output space (a 258-byte match plus the 32-byte store
// overshoot) and this much input (two refills per iteration, each advancing up to 7 bytes and
// then loading the next 8 bytes).
constexpr ptrdiff_t FAST_OUT_MARGIN = 258 + 32;
constexpr ptrdiff_t FAST_IN_MARGIN = 24;

static const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t kPrecodeOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// 10-bit bit-reversal table; the reversed codeword of length len <= 10 is kRev10[code] >> (10 - len).
struct Rev10 {
    uint16_t v[1024];
    constexpr Rev10() : v() {
        for (unsigned i = 0; i < 1024; i++) {
            unsigned r = 0;
            for (unsigned b = 0; b < 10; b++) r |= ((i >> b) & 1) << (9 - b);
            v[i] = (uint16_t) r;
        }
    }
};
static constexpr Rev10 kRev10;
static MI_INLINE unsigned reverseCode(unsigned code, unsigned len) {   // len <= 15
    if (len <= 10) return kRev10.v[code] >> (10 - len);
    return (((unsigned) kRev10.v[code & 1023] << 5) | (kRev10.v[code >> 10] >> 5)) >> (15 - len);
}

static MI_INLINE uint64_t load64(const uint8_t *p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
#else
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
#endif
}
static MI_INLINE void store64(uint8_t *p, uint64_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (int i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (8 * i));
#else
    memcpy(p, &v, 8);
#endif
}
static MI_INLINE void copy16(uint8_t *d, const uint8_t *s) { memcpy(d, s, 16); }
static MI_INLINE void copy8(uint8_t *d, const uint8_t *s) { memcpy(d, s, 8); }

static MI_INLINE unsigned ctz64(uint64_t x) {   // x != 0
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned) __builtin_ctzll(x);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long r;
    _BitScanForward64(&r, x);
    return (unsigned) r;
#else
    unsigned r = 0;
    while (!(x & 1)) { x >>= 1; r++; }
    return r;
#endif
}
// Index of the next lookup: (saved >> consumed) & mask(width), both taken from the entry.
// BMI1 bextr does it in one 1-cycle instruction on AMD Zen; the portable form uses the
// already shifted bits and a compile-time mask.
template <bool BMI>
static MI_INLINE uint64_t nextIndex(uint64_t saved, uint64_t shifted, uint32_t e, unsigned mask) {
#if defined(MI_X86_BMI_DISPATCH)
    if (BMI) {
        uint64_t idx;
        __asm__("bextr %2, %1, %0" : "=r"(idx) : "r"(saved), "r"((uint64_t) e));
        return idx;
    }
#endif
    (void) saved;
    (void) e;
    return shifted & mask;
}
// bits >> (bits consumed by the entry). BMI2 shrx is non-destructive and takes the count from
// any register, which spares the compiler the copy of `saved` and the move into cl that a
// plain shr puts on the dependency chain.
template <bool BMI>
static MI_INLINE uint64_t consume(uint64_t saved, uint32_t e) {
#if defined(MI_X86_BMI_DISPATCH)
    if (BMI) {
        uint64_t r;
        __asm__("shrx %2, %1, %0" : "=r"(r) : "r"(saved), "r"((uint64_t) e));
        return r;
    }
#endif
    return saved >> (e & 63);
}

// Calls fn(sym) for every sym < n with lens[sym] == len (there are `count` of them), in
// increasing order. lens must be readable up to n rounded up to 64 (the callers' arrays carry
// LENS_SLACK). Stops as soon as all `count` symbols were found.
template <class F>
static MI_INLINE void scanLength(const uint8_t *lens, unsigned n, unsigned len, unsigned count, F fn) {
    for (unsigned base = 0; base < n; base += 64) {
        const uint8_t *p = lens + base;
#ifdef MI_SSE2
        const __m128i v = _mm_set1_epi8((char) len);
        uint64_t m = (uint64_t) (unsigned) _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *) p), v))
                   | (uint64_t) (unsigned) _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *) (p + 16)), v)) << 16
                   | (uint64_t) (unsigned) _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *) (p + 32)), v)) << 32
                   | (uint64_t) (unsigned) _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *) (p + 48)), v)) << 48;
#else
        // SWAR: one bit per byte at the byte's top bit, then gathered to 8 bits per word.
        const uint64_t pat = 0x0101010101010101ull * len;
        uint64_t m = 0;
        for (unsigned k = 0; k < 8; k++) {
            uint64_t x = load64(p + 8 * k) ^ pat;                     // zero byte == match
            uint64_t z = ~(((x & 0x7f7f7f7f7f7f7f7full) + 0x7f7f7f7f7f7f7f7full) | x | 0x7f7f7f7f7f7f7f7full);   // exact
            m |= ((z >> 7) * 0x0102040810204080ull >> 56) << (8 * k);
        }
#endif
        if (base + 64 > n) m &= (1ull << (n - base)) - 1;
        while (m) {
            fn(base + ctz64(m));
            m &= m - 1;
            count--;
        }
        if (!count) break;
    }
}

// Entry makers: (symbol, bits this entry consumes) -> entry
struct LitLenEntry {
    MI_INLINE uint32_t operator()(unsigned sym, unsigned len) const {
        if (sym < 256) return TYPE_LIT | (sym << 16) | (LITLEN_BITS << 8) | len;
        if (sym == 256) return TYPE_END | (LITLEN_BITS << 8) | len;
        if (sym > 285) return TYPE_END | (1u << 16) | (LITLEN_BITS << 8) | len;
        unsigned nx = kLenExtra[sym - 257];
        return TYPE_LEN | (nx << 25) | ((uint32_t) kLenBase[sym - 257] << 16) | (DIST_BITS << 8) | (len + nx);
    }
};
struct DistEntry {
    MI_INLINE uint32_t operator()(unsigned sym, unsigned len) const {
        if (sym > 29) return TYPE_END | (1u << 16) | (LITLEN_BITS << 8) | len;
        return TYPE_LEN | (sym << 16) | (LITLEN_BITS << 8) | (len + kDistExtra[sym]);
    }
};
struct PrecodeEntry {
    MI_INLINE uint32_t operator()(unsigned sym, unsigned len) const { return (sym << 16) | (PRECODE_BITS << 8) | len; }
};

static MI_INLINE void countLengths(const uint8_t *lens, unsigned n, unsigned *count) {
    for (unsigned i = 0; i < 16; i++) count[i] = 0;
    for (unsigned i = 0; i < n; i++) count[lens[i]]++;
}

// Canonical Huffman table build from code lengths lens[0..n) (0 = unused) and their histogram
// count[1..15]. Returns false when the code is invalid the way zlib's inflate_table rejects
// it. With allowIncomplete an empty code (no symbols: every entry invalid) or a single code of
// length 1 is accepted.
template <int TB, class Mk>   // TB <= 10 (kRev10)
static MI_NOINLINE bool buildTable(const uint8_t *lens, unsigned n, const unsigned *countIn, uint32_t *table, unsigned cap, Mk mk, bool allowIncomplete) {
    unsigned count[16];
    unsigned left = 1, maxLen = 0;
    for (unsigned len = 1; len <= 15; len++) {
        count[len] = countIn[len];
        left <<= 1;
        if (count[len] > left) return false;                      // over-subscribed
        left -= count[len];
        if (count[len]) maxLen = len;
    }
    if (left != 0) {                                              // incomplete
        if (!allowIncomplete || maxLen > 1) return false;
        const uint32_t bad = TYPE_END | (1u << 16) | (LITLEN_BITS << 8) | 1;
        for (unsigned i = 0; i < (1u << TB); i++) table[i] = bad;
        if (maxLen == 1) {                                        // exactly one code: "0"
            unsigned sym = 0;
            while (lens[sym] == 0) sym++;
            uint32_t e = mk(sym, 1);
            for (unsigned i = 0; i < (1u << TB); i += 2) table[i] = e;
        }
        return true;
    }
    // Primary table: for each length the region [0, 1 << len) holds every code of length <= len
    // (replicated); moving to the next length doubles it. `code` is the canonical codeword in
    // normal bit order (next length: shift left), the table index is its bit reversal.
    unsigned code = 0;
    for (unsigned len = 1; len <= (unsigned) TB; len++) {
        if (count[len])
            scanLength(lens, n, len, count[len], [&](unsigned sym) {
                table[kRev10.v[code] >> (10 - len)] = mk(sym, len);
                code++;
            });
        code <<= 1;
        if (len < (unsigned) TB && len >= 3) {
            uint8_t *dst = (uint8_t *) (table + (1u << len));
            const uint8_t *src = (const uint8_t *) table;
            for (size_t bytes = (size_t) (1u << len) * sizeof(uint32_t), i = 0; i < bytes; i += 32) {
                copy16(dst + i, src + i);
                copy16(dst + i + 16, src + i + 16);
            }
        } else if (len == 2) {
            copy16((uint8_t *) (table + 4), (const uint8_t *) table);
        } else if (len == 1) {
            table[2] = table[0];
            table[3] = table[1];
        }
    }
    if (maxLen <= (unsigned) TB) return true;
    // Subtables for the longer codes: one per distinct TB-bit prefix, sized like zlib does it.
    unsigned subOff = 1u << TB, subBase = 0, subBits = 0, prefix = ~0u;
    bool ok = true;
    for (unsigned len = TB + 1; len <= maxLen; len++, code <<= 1) {
        if (!count[len]) continue;
        scanLength(lens, n, len, count[len], [&](unsigned sym) {
            unsigned codeword = reverseCode(code, len);
            code++;
            unsigned low = codeword & ((1u << TB) - 1);
            if (low != prefix) {
                prefix = low;
                unsigned curr = len - TB, l = len;
                int space = 1 << curr;
                while (l < maxLen) {
                    space -= (int) count[l];
                    if (space <= 0) break;
                    curr++;
                    space <<= 1;
                    l++;
                }
                subBits = curr;
                subBase = subOff;
                subOff += 1u << curr;
                if (subOff > cap) { ok = false; subOff = subBase; }
                table[prefix] = TYPE_SUB | (subBase << 16) | (subBits << 8) | (unsigned) TB;
            }
            uint32_t e = mk(sym, len - TB);                       // the primary lookup consumed TB bits
            for (unsigned i = codeword >> TB, stride = 1u << (len - TB); i < (1u << subBits); i += stride) table[subBase + i] = e;
            count[len]--;
        });
        if (!ok) return false;
    }
    return true;
}

struct FixedTables {
    uint32_t litlen[LITLEN_TABLE];
    uint32_t dist[DIST_TABLE];
    FixedTables() {
        uint8_t lens[288 + LENS_SLACK] = {0};                     // slack for the 64-byte scans
        unsigned count[16];
        unsigned i = 0;
        for (; i < 144; i++) lens[i] = 8;
        for (; i < 256; i++) lens[i] = 9;
        for (; i < 280; i++) lens[i] = 7;
        for (; i < 288; i++) lens[i] = 8;
        countLengths(lens, 288, count);
        buildTable<LITLEN_BITS>(lens, 288, count, litlen, LITLEN_TABLE, LitLenEntry(), false);
        for (i = 0; i < 32; i++) lens[i] = 5;
        countLengths(lens, 32, count);
        buildTable<DIST_BITS>(lens, 32, count, dist, DIST_TABLE, DistEntry(), false);
    }
};
static const FixedTables &fixedTables() {
    static const FixedTables t;
    return t;
}

// Bit reader state. Bits above `nbits` in `bits` are either zero or the true upcoming input
// bits (a refill leaves up to one uncounted byte there), so ORing a refill in is idempotent.
// `overread` counts virtual zero bytes appended once the input is exhausted. nbits is exact
// here; the decode loops keep garbage above bit 5 and mask before storing it back.
struct Reader {
    const uint8_t *in;
    const uint8_t *end;
    uint64_t bits;
    unsigned nbits;
    unsigned overread;
};

// Refill to 56..63 valid bits; the caller guarantees 8 readable bytes at `in`.
static MI_INLINE void refillFast(const uint8_t *&in, uint64_t &bits, unsigned &nbits) {
    bits |= load64(in) << (nbits & 63);
    in += 7 - ((nbits >> 3) & 7);
    nbits |= 56;
}
// Fast-loop refill from a pre-loaded word: `next` holds the 8 bytes at `in`, loaded right
// after the previous refill when the address was already known, so the refill's input load
// no longer waits for the nbits of the entries decoded since (that put the load's latency
// onto the dependency chain once per refill). The caller guarantees 8 readable bytes at the
// advanced `in`.
static MI_INLINE void refillPre(const uint8_t *&in, uint64_t &bits, unsigned &nbits, uint64_t &next) {
    bits |= next << (nbits & 63);
    in += 7 - ((nbits >> 3) & 7);
    nbits |= 56;
    next = load64(in);
}
// Checked refill: the last 7 input bytes are assembled into a word so the same accounting
// applies; bytes counted past the end are virtual zeros. Kept inline on purpose: passing the
// state by reference to an out-of-line helper forces it into memory, and every byte store
// through `out` would then reload it.
static MI_INLINE void refill(const uint8_t *&in, const uint8_t *end, uint64_t &bits, unsigned &nbits, unsigned &overread) {
    if (MI_LIKELY(end - in >= 8)) {
        refillFast(in, bits, nbits);
    } else {
        unsigned rem = (unsigned) (end - in);
        uint64_t w = 0;
        for (unsigned i = 0; i < rem; i++) w |= (uint64_t) in[i] << (8 * i);
        bits |= w << (nbits & 63);
        unsigned take = 7 - ((nbits >> 3) & 7);
        nbits |= 56;
        if (take <= rem) {
            in += take;
        } else {
            in = end;
            overread += take - rem;
        }
    }
}

// Real input bits not consumed yet (negative: virtual zero bits were consumed -> error).
static MI_INLINE ptrdiff_t availableBits(const Reader &r) { return (r.end - r.in) * 8 + (ptrdiff_t) r.nbits - (ptrdiff_t) r.overread * 8; }

// Extra bits of a length/distance entry: the top `nextra` bits of the `total` bits consumed
// (the code comes first in the stream). total <= 28, so after the left shift the low 36 bits
// are zero and for nextra == 0 (right shift by 0) the 32-bit truncation yields 0.
static MI_INLINE unsigned extraBits(uint64_t saved, unsigned total, unsigned nextra) {
    return (unsigned) ((saved << ((64 - total) & 63)) >> ((64 - nextra) & 63));
}

// Match copy for the fast loop: writes up to 31 bytes past out + len (within the margin).
static MI_INLINE void copyMatch(uint8_t *out, unsigned dist, unsigned len) {
    const uint8_t *src = out - dist;
    uint8_t *const stop = out + len;
    if (MI_LIKELY(dist >= 16)) {
        copy16(out, src);
        copy16(out + 16, src + 16);
        if (MI_UNLIKELY(len > 32)) {
            out += 32;
            src += 32;
            do {
                copy16(out, src);
                copy16(out + 16, src + 16);
                out += 32;
                src += 32;
            } while (out < stop);
        }
    } else if (dist >= 8) {
        copy8(out, src);
        copy8(out + 8, src + 8);
        copy8(out + 16, src + 16);
        copy8(out + 24, src + 24);
        if (MI_UNLIKELY(len > 32)) {
            out += 32;
            src += 32;
            do {
                copy8(out, src);
                out += 8;
                src += 8;
            } while (out < stop);
        }
    } else {
        // Write 8 bytes of the pattern byte by byte, then copy 8 bytes at a time from a stride
        // that is a multiple of dist and at least 8 (the source is then fully written and,
        // as dist <= bytes produced, inside the buffer).
        static const uint8_t kStride[8] = {0, 8, 8, 9, 8, 10, 12, 14};
        for (int i = 0; i < 8; i++) out[i] = src[i];
        unsigned s = kStride[dist];
        out += 8;
        while (out < stop) {
            copy8(out, out - s);
            out += 8;
        }
    }
}

// Decodes one Huffman block. Returns the new output pointer or nullptr on error.
//
// Fast loop (after libdeflate): the entry for the current position is always looked up
// before the refill that follows it, so the refill's input load and OR run beside the table
// lookup instead of ahead of it on the dependency chain. Every entry, subtable pointers
// included, is consumed (bits >>= total) before being dispatched. Bit budget with 56..63
// bits after each refill: four primary-table literals (<= 40 bits) still leave the 10 bits of
// the next preload; two literals plus a length code with extra bits (<= 40) leave the 8 bits
// of the distance preload; a distance (<= 28 bits) leaves the next litlen preload.
template <bool BMI>
static uint8_t *decodeBlock(Reader &r, const uint32_t *L, const uint32_t *D, uint8_t *outStart, uint8_t *out, uint8_t *outEnd) {
    const uint8_t *in = r.in, *end = r.end;
    const uint8_t *const realEnd = r.end;
    uint64_t bits = r.bits;
    unsigned nbits = r.nbits, overread = r.overread;
    constexpr unsigned LM = (1u << LITLEN_BITS) - 1, DM = (1u << DIST_BITS) - 1;
    uint8_t *const fastOutEnd = outEnd - out > FAST_OUT_MARGIN ? outEnd - FAST_OUT_MARGIN : out;
    // Once fewer than FAST_IN_MARGIN input bytes remain, the fast loop continues over a
    // zero-padded copy of them (TAIL_PAD bytes of padding: two refills past the loop
    // condition plus the 8-byte preload); whatever it counts past the real end becomes
    // `overread` afterwards.
    constexpr ptrdiff_t TAIL_PAD = 64;
    uint8_t tail[FAST_IN_MARGIN + TAIL_PAD];
    bool onTail = false;
    uint32_t e;
    uint64_t saved, next;

    for (;;) {
        const uint8_t *fastInEnd;
        if (!onTail && end - in <= FAST_IN_MARGIN) {
            onTail = true;
            ptrdiff_t rem = end - in;
            memset(tail, 0, sizeof(tail));
            memcpy(tail, in, (size_t) rem);
            in = tail;
            end = tail + rem;
        }
        fastInEnd = onTail ? end + FAST_IN_MARGIN : end - FAST_IN_MARGIN;
        if (in >= fastInEnd || out >= fastOutEnd) break;
        refill(in, end, bits, nbits, overread);
        next = load64(in);
        e = L[bits & LM];
        // Invariant at the top: e is the entry at the current position, >= 56 bits buffered,
        // next = the 8 bytes at in.
        do {
            saved = bits;
            bits = consume<BMI>(bits, e);
            nbits -= e;
            if (e < TYPE_LEN) {                                   // literal
                out[0] = (uint8_t) (e >> 16);
                e = L[nextIndex<BMI>(saved, bits, e, LM)];
                saved = bits;
                bits = consume<BMI>(bits, e);
                nbits -= e;
                if (e < TYPE_LEN) {
                    out[1] = (uint8_t) (e >> 16);
                    e = L[nextIndex<BMI>(saved, bits, e, LM)];
                    saved = bits;
                    bits = consume<BMI>(bits, e);
                    nbits -= e;
                    if (e < TYPE_LEN) {
                        out[2] = (uint8_t) (e >> 16);
                        e = L[nextIndex<BMI>(saved, bits, e, LM)];
                        saved = bits;
                        bits = consume<BMI>(bits, e);
                        nbits -= e;
                        if (e < TYPE_LEN) {
                            out[3] = (uint8_t) (e >> 16);
                            e = L[nextIndex<BMI>(saved, bits, e, LM)];
                            out += 4;
                            refillPre(in, bits, nbits, next);
                            continue;
                        }
                        out += 3;
                    } else {
                        out += 2;
                    }
                } else {
                    out += 1;
                }
            }
            if (MI_UNLIKELY(e & EXCEPTIONAL)) {
                if (e & (1u << 30)) goto blockEnd;                // TYPE_END
                e = L[((e >> 16) & VALUE_MASK) + nextIndex<BMI>(saved, bits, e, (1u << ((e >> 8) & 15)) - 1)];
                saved = bits;
                bits = consume<BMI>(bits, e);
                nbits -= e;
                if (e < TYPE_LEN) {
                    *out++ = (uint8_t) (e >> 16);
                    e = L[nextIndex<BMI>(saved, bits, e, LM)];
                    refillPre(in, bits, nbits, next);
                    continue;
                }
                if (e & EXCEPTIONAL) goto blockEnd;               // a subtable holds no pointers: TYPE_END
            }
            {
                unsigned len = ((e >> 16) & 0x1ff) + extraBits(saved, e & 63, (e >> 25) & 15);
                uint32_t d = D[nextIndex<BMI>(saved, bits, e, DM)];
                refillPre(in, bits, nbits, next);
                saved = bits;
                bits = consume<BMI>(bits, d);
                nbits -= d;
                if (MI_UNLIKELY(d & EXCEPTIONAL)) {
                    if (d & (1u << 30)) return nullptr;           // invalid distance code
                    d = D[((d >> 16) & VALUE_MASK) + nextIndex<BMI>(saved, bits, d, (1u << ((d >> 8) & 15)) - 1)];
                    saved = bits;
                    bits = consume<BMI>(bits, d);
                    nbits -= d;
                    if (d & EXCEPTIONAL) return nullptr;
                }
                unsigned dsym = (d >> 16) & 31;
                unsigned dist = kDistBase[dsym] + extraBits(saved, d & 63, kDistExtra[dsym]);
                e = L[nextIndex<BMI>(saved, bits, d, LM)];
                refillPre(in, bits, nbits, next);
                if (MI_UNLIKELY((ptrdiff_t) dist > out - outStart)) return nullptr;
                copyMatch(out, dist, len);
                out += len;
            }
        } while (in < fastInEnd && out < fastOutEnd);
        // Leaving the fast loop with e preloaded but not consumed: the slow loop re-reads it.
        if (onTail || out >= fastOutEnd) break;
    }
    if (in > end) {                                               // counted padding bytes
        overread += (unsigned) (in - end);
        in = end;
    }
    // Slow loop: checked refills, every write checked against outEnd. A valid stream never
    // counts more than 7 virtual bytes (they sit unconsumed in the bit buffer), so a larger
    // overread means the input ended inside the block: fail now rather than after filling
    // the output buffer with what the virtual zeros decode to.
    for (;;) {
        if (overread > 8) return nullptr;
        refill(in, end, bits, nbits, overread);
        e = L[bits & LM];
        saved = bits;
        bits = consume<BMI>(bits, e);
        nbits -= e;
        if ((e & EXCEPTIONAL) && !(e & (1u << 30))) {             // subtable pointer
            e = L[((e >> 16) & VALUE_MASK) + (bits & ((1u << ((e >> 8) & 15)) - 1))];
            saved = bits;
            bits = consume<BMI>(bits, e);
            nbits -= e;
        }
        if (e < TYPE_LEN) {
            if (out >= outEnd) return nullptr;
            *out++ = (uint8_t) (e >> 16);
            continue;
        }
        if (e & EXCEPTIONAL) break;                               // TYPE_END
        unsigned len = ((e >> 16) & 0x1ff) + extraBits(saved, e & 63, (e >> 25) & 15);
        refill(in, end, bits, nbits, overread);
        uint32_t d = D[bits & DM];
        saved = bits;
        bits = consume<BMI>(bits, d);
        nbits -= d;
        if ((d & EXCEPTIONAL) && !(d & (1u << 30))) {
            d = D[((d >> 16) & VALUE_MASK) + (bits & ((1u << ((d >> 8) & 15)) - 1))];
            saved = bits;
            bits = consume<BMI>(bits, d);
            nbits -= d;
        }
        if (d & EXCEPTIONAL) return nullptr;
        unsigned dsym = (d >> 16) & 31;
        unsigned dist = kDistBase[dsym] + extraBits(saved, d & 63, kDistExtra[dsym]);
        if ((ptrdiff_t) dist > out - outStart || (ptrdiff_t) len > outEnd - out) return nullptr;
        const uint8_t *src = out - dist;
        for (unsigned i = 0; i < len; i++) out[i] = src[i];
        out += len;
    }
blockEnd:
    if ((e >> 16) & 1) return nullptr;                            // invalid code
    if (in > end) {
        overread += (unsigned) (in - end);
        in = end;
    }
    r.in = realEnd - (end - in); r.bits = bits; r.nbits = nbits & 63; r.overread = overread;
    return out;
}

// Reads the dynamic block header and builds the tables. Returns false on error.
template <bool BMI>
static MI_NOINLINE bool readDynamicHeader(Reader &r, uint32_t *litlen, uint32_t *dist) {
    const uint8_t *in = r.in, *const end = r.end;
    uint64_t bits = r.bits;
    unsigned nbits = r.nbits, overread = r.overread;
    refill(in, end, bits, nbits, overread);
    unsigned hlit = (unsigned) (bits & 31) + 257, hdist = (unsigned) ((bits >> 5) & 31) + 1, hclen = (unsigned) ((bits >> 10) & 15) + 4;
    bits >>= 14;
    nbits -= 14;
    if (hlit > 286 || hdist > 30) return false;
    uint8_t clens[19 + LENS_SLACK] = {0};
    for (unsigned i = 0; i < hclen; i++) {
        if (i == 14) refill(in, end, bits, nbits, overread);
        clens[kPrecodeOrder[i]] = (uint8_t) (bits & 7);
        bits >>= 3;
        nbits -= 3;
    }
    uint32_t precode[PRECODE_TABLE];
    unsigned pcount[16];
    countLengths(clens, 19, pcount);
    if (!buildTable<PRECODE_BITS>(clens, 19, pcount, precode, PRECODE_TABLE, PrecodeEntry(), false)) return false;
    // Code lengths, with the litlen and distance histograms accumulated on the way (a repeat
    // run crossing hlit is split between them). The entry for the next symbol is looked up
    // before the refill; three symbols (7 code + up to 7 extra bits each) per refill keep the
    // preload within the 56 guaranteed bits.
    constexpr unsigned PM = PRECODE_TABLE - 1;
    uint8_t lensBuf[1 + MAX_LENS + REP_SLACK + LENS_SLACK];
    uint8_t *const lens = lensBuf + 1;                            // lens[-1] readable for sym 16 at i == 0
    lensBuf[0] = 0;
    unsigned countL[16], countD[16];
    for (unsigned i = 0; i < 16; i++) countL[i] = countD[i] = 0;
    const unsigned n = hlit + hdist;
    unsigned i = 0;
    refill(in, end, bits, nbits, overread);
    uint32_t e = precode[bits & PM];
    while (i < n) {
        for (int k = 0; k < 3 && i < n; k++) {
            unsigned sym = e >> 16;
            uint64_t saved = bits;
            bits = consume<BMI>(bits, e);
            nbits -= e & 63;
            if (MI_LIKELY(sym < 16)) {
                lens[i] = (uint8_t) sym;
                (i < hlit ? countL : countD)[sym]++;
                i++;
                e = precode[nextIndex<BMI>(saved, bits, e, PM)];
                continue;
            }
            unsigned rep, val = 0, nx;
            if (sym == 16) {
                if (i == 0) return false;
                val = lens[i - 1];
                rep = 3 + (unsigned) (bits & 3);
                nx = 2;
            } else if (sym == 17) {
                rep = 3 + (unsigned) (bits & 7);
                nx = 3;
            } else {
                rep = 11 + (unsigned) (bits & 127);
                nx = 7;
            }
            bits >>= nx;
            nbits -= nx;
            e = precode[bits & PM];
            uint64_t pat = 0x0101010101010101ull * val;
            for (unsigned j = 0; j < rep; j += 8) store64(lens + i + j, pat);   // may overshoot into the slack
            if (val) {
                if (i + rep <= hlit) countL[val] += rep;
                else if (i >= hlit) countD[val] += rep;
                else { countL[val] += hlit - i; countD[val] += i + rep - hlit; }
            }
            i += rep;
            if (i > n) return false;
        }
        refill(in, end, bits, nbits, overread);
    }
    r.in = in; r.bits = bits; r.nbits = nbits; r.overread = overread;
    if (lens[256] == 0) return false;                             // no end-of-block code
    if (!buildTable<LITLEN_BITS>(lens, hlit, countL, litlen, LITLEN_TABLE, LitLenEntry(), true)) return false;
    if (!buildTable<DIST_BITS>(lens + hlit, hdist, countD, dist, DIST_TABLE, DistEntry(), true)) return false;
    return true;
}

static inline bool haveBMI() {
#if defined(MI_X86_BMI_DISPATCH)
    static const bool v = (__builtin_cpu_init(), __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2"));
    return v;
#else
    return false;
#endif
}

template <bool BMI>
static size_t inflateImpl(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap) {
    Reader r{in, in + inLen, 0, 0, 0};
    uint8_t *o = out, *const outEnd = out + outCap;
    uint32_t litlen[LITLEN_TABLE], dist[DIST_TABLE];
    for (;;) {
        ptrdiff_t avail = availableBits(r);
        if (avail < 0) return FAIL;
        if (avail < 3) return (size_t) (o - out);                 // exhausted at a block boundary
        refill(r.in, r.end, r.bits, r.nbits, r.overread);
        unsigned hdr = (unsigned) (r.bits & 7);
        r.bits >>= 3;
        r.nbits -= 3;
        unsigned type = hdr >> 1;
        if (type == 0) {
            // Stored: align to a byte, give the buffered whole bytes back to the input.
            unsigned drop = r.nbits & 7;
            r.bits >>= drop;
            r.nbits -= drop;
            ptrdiff_t buffered = (ptrdiff_t) (r.nbits >> 3) - (ptrdiff_t) r.overread;
            if (buffered < 0) return FAIL;
            r.in -= buffered;
            r.bits = 0;
            r.nbits = 0;
            r.overread = 0;
            ptrdiff_t left = r.end - r.in;
            if (left == 0) return (size_t) (o - out);             // the stripped sync-flush tail
            if (left < 4) return FAIL;
            unsigned len = r.in[0] | (r.in[1] << 8), nlen = r.in[2] | (r.in[3] << 8);
            if (len != (~nlen & 0xffff)) return FAIL;
            r.in += 4;
            if ((ptrdiff_t) len > r.end - r.in || (ptrdiff_t) len > outEnd - o) return FAIL;
            memcpy(o, r.in, len);
            r.in += len;
            o += len;
        } else if (type == 3) {
            return FAIL;
        } else {
            const uint32_t *L, *D;
            if (type == 1) {
                const FixedTables &f = fixedTables();
                L = f.litlen;
                D = f.dist;
            } else {
                if (!readDynamicHeader<BMI>(r, litlen, dist)) return FAIL;
                L = litlen;
                D = dist;
            }
            o = decodeBlock<BMI>(r, L, D, out, o, outEnd);
            if (!o) return FAIL;
            if (availableBits(r) < 0) return FAIL;                // consumed past the input
        }
        if (hdr & 1) return (size_t) (o - out);                   // BFINAL
    }
}

}   // namespace detail

inline size_t inflate(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap) {
    return detail::haveBMI() ? detail::inflateImpl<true>(in, inLen, out, outCap) : detail::inflateImpl<false>(in, inLen, out, outCap);
}

}   // namespace microinflate
}   // namespace cWS
