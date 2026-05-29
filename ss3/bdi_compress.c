/* bdi_compress.c - Base-Delta-Immediate (BDI) cache compression */

/*
 * BDI compression as described in:
 *   Pekhimenko et al., "Base-Delta-Immediate Compression: Practical Data
 *   Compression for On-Chip Caches", PACT 2012.
 *
 * Original POC by Mohammed (poc/bdi_cache_test.c, poc/bdi_decompression.c).
 * Adapted for SimpleScalar integration by extracting the pure compression
 * analysis logic and giving it a clean, simulator-friendly API.
 *
 * Changes from the POC:
 *   - Removed all cache-structure code (that lives in cache.c / cache.h).
 *   - Each check_*() helper now returns bool and writes the zero-bitmask
 *     through an output pointer, avoiding the comp_data_t intermediate.
 *   - bdi_compress() is the single public entry point.
 *   - Fixed the B4-D2 label typo present in poc/bdi_cache_test.c line 50.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include "bdi_compress.h"

/* -------------------------------------------------------------------------
 * Metadata tables
 * ---------------------------------------------------------------------- */

const char *bdi_type_names[BDI_NUM_TYPES] = {
    [BDI_ZERO]    = "ZERO",
    [BDI_REP_VAL] = "REP_VAL",
    [BDI_B8_D1]   = "B8-D1",
    [BDI_B4_D1]   = "B4-D1",
    [BDI_B8_D2]   = "B8-D2",
    [BDI_B2_D1]   = "B2-D1",
    [BDI_B4_D2]   = "B4-D2",   /* fixed: POC had "B4-D1" here by mistake */
    [BDI_B8_D4]   = "B8-D4",
    [BDI_NONE]    = "NONE"
};

/* Compressed byte count for each encoding (64-byte source block) */
static const int bdi_sizes[BDI_NUM_TYPES] = {
    [BDI_ZERO]    =  1,
    [BDI_REP_VAL] =  8,
    [BDI_B8_D1]   = 16,
    [BDI_B4_D1]   = 20,
    [BDI_B8_D2]   = 24,
    [BDI_B2_D1]   = 34,
    [BDI_B4_D2]   = 36,
    [BDI_B8_D4]   = 40,
    [BDI_NONE]    = 64
};

int bdi_compressed_size(bdi_type_t type)
{
    if (type < 0 || type >= BDI_NUM_TYPES)
        return BDI_BLOCK_SIZE;
    return bdi_sizes[type];
}

/* -------------------------------------------------------------------------
 * Internal per-encoding checkers
 *
 * Each function returns true if the 64-byte block DATA can be represented
 * by that encoding, and writes the zero-bitmask into *ZMASK_OUT when true.
 *
 * zero-bitmask convention (matches Mohammed's compressor and decompressor):
 *   bit SET (1)   -> element fits in delta range of the zero base; use delta
 *   bit CLEAR (0) -> element uses the arbitrary base + delta
 *   MSB-first per element count N: element i occupies bit (N-1-i).
 * ---------------------------------------------------------------------- */

/* ZERO: every byte is 0 */
static bool check_zero(const uint8_t *data)
{
    for (int i = 0; i < BDI_BLOCK_SIZE; i++)
        if (data[i]) return false;
    return true;
}

/* REP_VAL: the 8-byte pattern data[0..7] repeats across the entire block */
static bool check_rep_val(const uint8_t *data)
{
    for (int i = 8; i < BDI_BLOCK_SIZE; i++)
        if (data[i] != data[i & 7]) return false;
    return true;
}

/*
 * B8_D1: 8-byte base, eight 1-byte signed deltas.
 * Elements: 8 x 8-byte values read big-endian.
 * N=8, element i at bit (7-i) of a uint8_t bitmask stored in int32_t.
 */
static bool check_B8D1(const uint8_t *data, int32_t *zmask_out)
{
    uint64_t values[8];
    int32_t  zmask    = 0;
    int64_t  base     = 0;
    bool     base_set = false;

    for (int i = 0; i < 8; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++)
            v = (v << 8) | data[i*8 + j];
        values[i] = v;

        int64_t sv = (int64_t)v;
        if (sv >= -128 && sv <= 127)
            zmask |= (0x80 >> i);           /* fits as zero-base delta */
        else if (!base_set) {
            base = sv;
            base_set = true;
        }
    }

    /* second pass: verify all non-zero-base elements fit in 1-byte delta */
    for (int i = 0; i < 8; i++) {
        if (zmask & (0x80 >> i)) continue;
        int64_t d = (int64_t)values[i] - base;
        if (d < -128 || d > 127) return false;
    }

    if (zmask_out) *zmask_out = zmask;
    return true;
}

/*
 * B4_D1: 4-byte base, sixteen 1-byte signed deltas.
 * Elements: 16 x 4-byte values read big-endian.
 * N=16, element i at bit (15-i).
 */
static bool check_B4D1(const uint8_t *data, int32_t *zmask_out)
{
    uint32_t values[16];
    int32_t  zmask    = 0;
    int32_t  base     = 0;
    bool     base_set = false;

    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++)
            v = (v << 8) | data[i*4 + j];
        values[i] = v;

        int32_t sv = (int32_t)v;
        if (sv >= -128 && sv <= 127)
            zmask |= (int32_t)(0x8000 >> i);
        else if (!base_set) {
            base = sv;
            base_set = true;
        }
    }

    for (int i = 0; i < 16; i++) {
        if (zmask & (int32_t)(0x8000 >> i)) continue;
        int32_t d = (int32_t)values[i] - base;
        if (d < -128 || d > 127) return false;
    }

    if (zmask_out) *zmask_out = zmask;
    return true;
}

/*
 * B8_D2: 8-byte base, eight 2-byte signed deltas.
 * N=8, element i at bit (7-i).
 */
static bool check_B8D2(const uint8_t *data, int32_t *zmask_out)
{
    uint64_t values[8];
    int32_t  zmask    = 0;
    int64_t  base     = 0;
    bool     base_set = false;

    for (int i = 0; i < 8; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++)
            v = (v << 8) | data[i*8 + j];
        values[i] = v;

        int64_t sv = (int64_t)v;
        if (sv >= -32768 && sv <= 32767)
            zmask |= (0x80 >> i);
        else if (!base_set) {
            base = sv;
            base_set = true;
        }
    }

    for (int i = 0; i < 8; i++) {
        if (zmask & (0x80 >> i)) continue;
        int64_t d = (int64_t)values[i] - base;
        if (d < -32768 || d > 32767) return false;
    }

    if (zmask_out) *zmask_out = zmask;
    return true;
}

/*
 * B2_D1: 2-byte base, thirty-two 1-byte signed deltas.
 * Elements: 32 x 2-byte values read big-endian.
 * N=32, element i at bit (31-i).  Cast to uint32_t before shifting to
 * avoid undefined behaviour on right-shifting a negative int32_t.
 */
static bool check_B2D1(const uint8_t *data, int32_t *zmask_out)
{
    uint16_t values[32];
    uint32_t zmask    = 0;
    int16_t  base     = 0;
    bool     base_set = false;

    for (int i = 0; i < 32; i++) {
        uint16_t v = ((uint16_t)data[i*2] << 8) | data[i*2 + 1];
        values[i] = v;

        int16_t sv = (int16_t)v;
        if (sv >= -128 && sv <= 127)
            zmask |= (0x80000000U >> i);
        else if (!base_set) {
            base = sv;
            base_set = true;
        }
    }

    for (int i = 0; i < 32; i++) {
        if (zmask & (0x80000000U >> i)) continue;
        int16_t d = (int16_t)((int16_t)values[i] - base);
        if (d < -128 || d > 127) return false;
    }

    if (zmask_out) *zmask_out = (int32_t)zmask;
    return true;
}

/*
 * B4_D2: 4-byte base, sixteen 2-byte signed deltas.
 * N=16, element i at bit (15-i).
 */
static bool check_B4D2(const uint8_t *data, int32_t *zmask_out)
{
    uint32_t values[16];
    int32_t  zmask    = 0;
    int32_t  base     = 0;
    bool     base_set = false;

    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++)
            v = (v << 8) | data[i*4 + j];
        values[i] = v;

        int32_t sv = (int32_t)v;
        if (sv >= -32768 && sv <= 32767)
            zmask |= (int32_t)(0x8000 >> i);
        else if (!base_set) {
            base = sv;
            base_set = true;
        }
    }

    for (int i = 0; i < 16; i++) {
        if (zmask & (int32_t)(0x8000 >> i)) continue;
        int32_t d = (int32_t)values[i] - base;
        if (d < -32768 || d > 32767) return false;
    }

    if (zmask_out) *zmask_out = zmask;
    return true;
}

/*
 * B8_D4: 8-byte base, eight 4-byte signed deltas.
 * N=8, element i at bit (7-i).
 */
static bool check_B8D4(const uint8_t *data, int32_t *zmask_out)
{
    uint64_t values[8];
    int32_t  zmask    = 0;
    int64_t  base     = 0;
    bool     base_set = false;

    for (int i = 0; i < 8; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++)
            v = (v << 8) | data[i*8 + j];
        values[i] = v;

        int64_t sv = (int64_t)v;
        if (sv >= INT32_MIN && sv <= INT32_MAX)
            zmask |= (0x80 >> i);
        else if (!base_set) {
            base = sv;
            base_set = true;
        }
    }

    for (int i = 0; i < 8; i++) {
        if (zmask & (0x80 >> i)) continue;
        int64_t d = (int64_t)values[i] - base;
        if (d < INT32_MIN || d > INT32_MAX) return false;
    }

    if (zmask_out) *zmask_out = zmask;
    return true;
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */

/*
 * bdi_compress - try each BDI encoding in priority order and return the
 * first one that covers the 64-byte block DATA.  The priority order matches
 * Mohammed's compressor: ZERO > REP_VAL > B8D1 > B4D1 > B8D2 > B2D1 >
 * B4D2 > B8D4 > NONE.
 */
bdi_type_t bdi_compress(const uint8_t *data, int32_t *zero_bitmask_out)
{
    int32_t zmask = 0;

    if (check_zero(data))
        return BDI_ZERO;

    if (check_rep_val(data))
        return BDI_REP_VAL;

    if (check_B8D1(data, &zmask)) {
        if (zero_bitmask_out) *zero_bitmask_out = zmask;
        return BDI_B8_D1;
    }
    if (check_B4D1(data, &zmask)) {
        if (zero_bitmask_out) *zero_bitmask_out = zmask;
        return BDI_B4_D1;
    }
    if (check_B8D2(data, &zmask)) {
        if (zero_bitmask_out) *zero_bitmask_out = zmask;
        return BDI_B8_D2;
    }
    if (check_B2D1(data, &zmask)) {
        if (zero_bitmask_out) *zero_bitmask_out = zmask;
        return BDI_B2_D1;
    }
    if (check_B4D2(data, &zmask)) {
        if (zero_bitmask_out) *zero_bitmask_out = zmask;
        return BDI_B4_D2;
    }
    if (check_B8D4(data, &zmask)) {
        if (zero_bitmask_out) *zero_bitmask_out = zmask;
        return BDI_B8_D4;
    }

    return BDI_NONE;
}

/* -------------------------------------------------------------------------
 * Decompression
 *
 * Ported from Mohammed's poc/bdi_decompression.c.
 * Adapted to use bdi_type_t instead of comp_type_t.
 * Logic and byte layout are identical to the original.
 * ---------------------------------------------------------------------- */

int bdi_decompress(bdi_type_t type, int32_t zero_bitmask,
                   const uint8_t *cb, uint8_t out[BDI_BLOCK_SIZE])
{
    switch (type) {

    case BDI_ZERO:
        memset(out, 0, BDI_BLOCK_SIZE);
        return 0;

    case BDI_REP_VAL: {
        /* cb[0..7]: repeated 8-byte value, big-endian */
        int64_t base = 0;
        int b;
        for (b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        {
            int64_t *o = (int64_t *)out;
            int i;
            for (i = 0; i < 8; i++)
                o[i] = base;
        }
        return 0;
    }

    case BDI_B8_D1: {
        /* cb[0..7]: 8-byte base BE, cb[8..15]: eight 1-byte deltas */
        int64_t base = 0;
        int64_t *o = (int64_t *)out;
        int b, i;
        for (b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        for (i = 0; i < 8; i++) {
            int8_t delta = (int8_t)cb[8 + i];
            o[i] = ((zero_bitmask >> (7 - i)) & 1) ? (int64_t)delta
                                                    : base + delta;
        }
        return 0;
    }

    case BDI_B8_D2: {
        /* cb[0..7]: 8-byte base BE, cb[8..23]: eight 2-byte deltas BE */
        int64_t base = 0;
        int64_t *o = (int64_t *)out;
        int b, i;
        for (b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        for (i = 0; i < 8; i++) {
            int16_t delta = (int16_t)((cb[8 + i*2] << 8) | cb[8 + i*2 + 1]);
            o[i] = ((zero_bitmask >> (7 - i)) & 1) ? (int64_t)delta
                                                    : base + delta;
        }
        return 0;
    }

    case BDI_B8_D4: {
        /* cb[0..7]: 8-byte base BE, cb[8..39]: eight 4-byte deltas BE */
        int64_t base = 0;
        int64_t *o = (int64_t *)out;
        int b, i;
        for (b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        for (i = 0; i < 8; i++) {
            int32_t delta = (int32_t)(  ((uint32_t)cb[8 + i*4]     << 24)
                                      | ((uint32_t)cb[8 + i*4 + 1] << 16)
                                      | ((uint32_t)cb[8 + i*4 + 2] <<  8)
                                      |  (uint32_t)cb[8 + i*4 + 3]);
            o[i] = ((zero_bitmask >> (7 - i)) & 1) ? (int64_t)delta
                                                    : base + delta;
        }
        return 0;
    }

    case BDI_B4_D1: {
        /* cb[0..3]: 4-byte base BE, cb[4..19]: sixteen 1-byte deltas */
        int32_t base = (int32_t)(  ((uint32_t)cb[0] << 24)
                                 | ((uint32_t)cb[1] << 16)
                                 | ((uint32_t)cb[2] <<  8)
                                 |  (uint32_t)cb[3]);
        int32_t *o = (int32_t *)out;
        int i;
        for (i = 0; i < 16; i++) {
            int8_t delta = (int8_t)cb[4 + i];
            o[i] = ((zero_bitmask >> (15 - i)) & 1) ? (int32_t)delta
                                                     : base + delta;
        }
        return 0;
    }

    case BDI_B4_D2: {
        /* cb[0..3]: 4-byte base BE, cb[4..35]: sixteen 2-byte deltas BE */
        int32_t base = (int32_t)(  ((uint32_t)cb[0] << 24)
                                 | ((uint32_t)cb[1] << 16)
                                 | ((uint32_t)cb[2] <<  8)
                                 |  (uint32_t)cb[3]);
        int32_t *o = (int32_t *)out;
        int i;
        for (i = 0; i < 16; i++) {
            int16_t delta = (int16_t)((cb[4 + i*2] << 8) | cb[4 + i*2 + 1]);
            o[i] = ((zero_bitmask >> (15 - i)) & 1) ? (int32_t)delta
                                                     : base + delta;
        }
        return 0;
    }

    case BDI_B2_D1: {
        /* cb[0..1]: 2-byte base BE, cb[2..33]: thirty-two 1-byte deltas */
        int16_t base = (int16_t)((cb[0] << 8) | cb[1]);
        int16_t *o   = (int16_t *)out;
        uint32_t zmask = (uint32_t)zero_bitmask;
        int i;
        for (i = 0; i < 32; i++) {
            int8_t delta = (int8_t)cb[2 + i];
            o[i] = ((zmask >> (31 - i)) & 1) ? (int16_t)delta
                                              : (int16_t)(base + delta);
        }
        return 0;
    }

    case BDI_NONE:
    default:
        /* Uncompressed — caller must memcpy raw bytes directly */
        return -1;
    }
}
