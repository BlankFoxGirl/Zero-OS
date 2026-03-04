#include "inflate.h"
#include "string.h"

// Minimal DEFLATE (RFC 1951) and gzip (RFC 1952) decompressor.
// No heap allocation — all state lives on the stack.

static constexpr uint32_t MAXBITS   = 15;
static constexpr uint32_t MAXLCODES = 286;
static constexpr uint32_t MAXDCODES = 30;
static constexpr uint32_t FIXLCODES = 288;

// ── Bit reader ───────────────────────────────────────────────────────

struct BitStream {
    const uint8_t *data;
    uint64_t       len;
    uint64_t       pos;
    uint32_t       buf;
    uint32_t       bits;
};

static void bs_init(BitStream *bs, const void *data, uint64_t len) {
    bs->data = static_cast<const uint8_t *>(data);
    bs->len  = len;
    bs->pos  = 0;
    bs->buf  = 0;
    bs->bits = 0;
}

static uint32_t bs_read(BitStream *bs, uint32_t n) {
    while (bs->bits < n) {
        if (bs->pos >= bs->len)
            return 0;
        bs->buf |= static_cast<uint32_t>(bs->data[bs->pos++]) << bs->bits;
        bs->bits += 8;
    }
    uint32_t val = bs->buf & ((1u << n) - 1);
    bs->buf >>= n;
    bs->bits -= n;
    return val;
}

static void bs_align(BitStream *bs) {
    bs->buf  = 0;
    bs->bits = 0;
}

// ── Huffman table ────────────────────────────────────────────────────

struct HuffTable {
    int16_t  counts[MAXBITS + 1];
    int16_t  symbols[MAXLCODES];
};

static bool huff_build(HuffTable *h, const int16_t *lengths, uint32_t n) {
    for (uint32_t i = 0; i <= MAXBITS; i++)
        h->counts[i] = 0;

    for (uint32_t i = 0; i < n; i++)
        h->counts[lengths[i]]++;

    if (h->counts[0] == static_cast<int16_t>(n))
        return true;

    int left = 1;
    for (uint32_t i = 1; i <= MAXBITS; i++) {
        left <<= 1;
        left -= h->counts[i];
        if (left < 0)
            return false;
    }

    int16_t offsets[MAXBITS + 1];
    offsets[1] = 0;
    for (uint32_t i = 1; i < MAXBITS; i++)
        offsets[i + 1] = offsets[i] + h->counts[i];

    for (uint32_t i = 0; i < n; i++) {
        if (lengths[i] != 0)
            h->symbols[offsets[lengths[i]]++] = static_cast<int16_t>(i);
    }

    return true;
}

static int huff_decode(BitStream *bs, const HuffTable *h) {
    int code  = 0;
    int first = 0;
    int index = 0;

    for (uint32_t len = 1; len <= MAXBITS; len++) {
        code |= static_cast<int>(bs_read(bs, 1));
        int count = h->counts[len];
        if (code - count < first)
            return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }

    return -1;
}

// ── Length / distance base tables ────────────────────────────────────

static const uint16_t len_base[] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint16_t len_extra[] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const uint16_t dist_extra[] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

// Code-length code order per RFC 1951 sec. 3.2.7
static const uint8_t cl_order[] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

// ── DEFLATE block decoder ────────────────────────────────────────────

struct InflateState {
    uint8_t *out;
    uint64_t out_pos;
    uint64_t out_cap;
};

static bool emit(InflateState *st, uint8_t byte) {
    if (st->out_pos >= st->out_cap)
        return false;
    st->out[st->out_pos++] = byte;
    return true;
}

static bool inflate_codes(BitStream *bs, InflateState *st,
                          const HuffTable *lencode,
                          const HuffTable *distcode) {
    for (;;) {
        int sym = huff_decode(bs, lencode);
        if (sym < 0)
            return false;

        if (sym < 256) {
            if (!emit(st, static_cast<uint8_t>(sym)))
                return false;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29)
                return false;
            uint32_t length = len_base[sym] + bs_read(bs, len_extra[sym]);

            int dsym = huff_decode(bs, distcode);
            if (dsym < 0 || dsym >= 30)
                return false;
            uint32_t dist = dist_base[dsym] + bs_read(bs, dist_extra[dsym]);

            if (static_cast<uint64_t>(dist) > st->out_pos)
                return false;

            uint64_t src_off = st->out_pos - dist;
            for (uint32_t i = 0; i < length; i++) {
                if (!emit(st, st->out[src_off + i]))
                    return false;
            }
        }
    }
}

static bool inflate_stored(BitStream *bs, InflateState *st) {
    bs_align(bs);
    uint32_t len  = bs_read(bs, 16);
    uint32_t nlen = bs_read(bs, 16);
    if ((len ^ 0xFFFF) != nlen)
        return false;

    for (uint32_t i = 0; i < len; i++) {
        if (!emit(st, static_cast<uint8_t>(bs_read(bs, 8))))
            return false;
    }
    return true;
}

static bool inflate_fixed(BitStream *bs, InflateState *st) {
    int16_t lengths[FIXLCODES];
    uint32_t i;
    for (i = 0;   i < 144; i++) lengths[i] = 8;
    for (;        i < 256; i++) lengths[i] = 9;
    for (;        i < 280; i++) lengths[i] = 7;
    for (;        i < FIXLCODES; i++) lengths[i] = 8;

    HuffTable lencode{}, distcode{};
    huff_build(&lencode, lengths, FIXLCODES);

    for (i = 0; i < MAXDCODES; i++) lengths[i] = 5;
    huff_build(&distcode, lengths, MAXDCODES);

    return inflate_codes(bs, st, &lencode, &distcode);
}

static bool inflate_dynamic(BitStream *bs, InflateState *st) {
    uint32_t nlen  = bs_read(bs, 5) + 257;
    uint32_t ndist = bs_read(bs, 5) + 1;
    uint32_t ncode = bs_read(bs, 4) + 4;

    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return false;

    int16_t lengths[MAXLCODES + MAXDCODES];
    memset(lengths, 0, sizeof(lengths));

    for (uint32_t i = 0; i < ncode; i++)
        lengths[cl_order[i]] = static_cast<int16_t>(bs_read(bs, 3));
    for (uint32_t i = ncode; i < 19; i++)
        lengths[cl_order[i]] = 0;

    HuffTable clcode{};
    if (!huff_build(&clcode, lengths, 19))
        return false;

    uint32_t total = nlen + ndist;
    uint32_t idx = 0;
    while (idx < total) {
        int sym = huff_decode(bs, &clcode);
        if (sym < 0)
            return false;

        if (sym < 16) {
            lengths[idx++] = static_cast<int16_t>(sym);
        } else if (sym == 16) {
            if (idx == 0) return false;
            int16_t prev = lengths[idx - 1];
            uint32_t rep = 3 + bs_read(bs, 2);
            for (uint32_t r = 0; r < rep && idx < total; r++)
                lengths[idx++] = prev;
        } else if (sym == 17) {
            uint32_t rep = 3 + bs_read(bs, 3);
            for (uint32_t r = 0; r < rep && idx < total; r++)
                lengths[idx++] = 0;
        } else {
            uint32_t rep = 11 + bs_read(bs, 7);
            for (uint32_t r = 0; r < rep && idx < total; r++)
                lengths[idx++] = 0;
        }
    }

    HuffTable lencode{}, distcode{};
    if (!huff_build(&lencode, lengths, nlen))
        return false;
    if (!huff_build(&distcode, lengths + nlen, ndist))
        return false;

    return inflate_codes(bs, st, &lencode, &distcode);
}

static uint64_t inflate_raw(const void *src, uint64_t src_len,
                            void *dst, uint64_t dst_cap) {
    BitStream bs;
    bs_init(&bs, src, src_len);

    InflateState st;
    st.out     = static_cast<uint8_t *>(dst);
    st.out_pos = 0;
    st.out_cap = dst_cap;

    uint32_t bfinal;
    do {
        bfinal = bs_read(&bs, 1);
        uint32_t btype = bs_read(&bs, 2);

        bool ok;
        switch (btype) {
        case 0: ok = inflate_stored(&bs, &st); break;
        case 1: ok = inflate_fixed(&bs, &st);  break;
        case 2: ok = inflate_dynamic(&bs, &st); break;
        default: return 0;
        }
        if (!ok)
            return 0;
    } while (!bfinal);

    return st.out_pos;
}

// ── Gzip wrapper ─────────────────────────────────────────────────────

uint64_t gzip_decompress(const void *src, uint64_t src_len,
                         void *dst, uint64_t dst_cap) {
    auto *p = static_cast<const uint8_t *>(src);
    if (src_len < 10)
        return 0;
    if (p[0] != 0x1F || p[1] != 0x8B)
        return 0;
    if (p[2] != 0x08)
        return 0;

    uint8_t flags = p[3];
    uint64_t off = 10;

    // FEXTRA
    if (flags & 0x04) {
        if (off + 2 > src_len) return 0;
        uint16_t xlen = static_cast<uint16_t>(p[off]) |
                        (static_cast<uint16_t>(p[off + 1]) << 8);
        off += 2 + xlen;
    }
    // FNAME
    if (flags & 0x08) {
        while (off < src_len && p[off] != 0) off++;
        off++;
    }
    // FCOMMENT
    if (flags & 0x10) {
        while (off < src_len && p[off] != 0) off++;
        off++;
    }
    // FHCRC
    if (flags & 0x02)
        off += 2;

    if (off >= src_len)
        return 0;

    return inflate_raw(p + off, src_len - off, dst, dst_cap);
}
