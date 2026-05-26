/*
 * bdi_decompress.c
 * Base-Delta-Immediate (BDI) cache decompression
 * Pekhimenko et al., PACT 2012
 *
 * Encodings (comp_type_t):
 *   COMP_TYPE_ZERO    - entire line is zero
 *   COMP_TYPE_REP_VAL - one value repeated across the line
 *   COMP_TYPE_B8_D1   - 8-byte base, 1-byte deltas (8 elements)
 *   COMP_TYPE_B8_D2   - 8-byte base, 2-byte deltas (8 elements)
 *   COMP_TYPE_B8_D4   - 8-byte base, 4-byte deltas (8 elements)
 *   COMP_TYPE_B4_D1   - 4-byte base, 1-byte deltas (16 elements)
 *   COMP_TYPE_B4_D2   - 4-byte base, 2-byte deltas (16 elements)
 *   COMP_TYPE_B2_D1   - 2-byte base, 1-byte deltas (32 elements)
 *   COMP_TYPE_NONE    - raw 64-byte line (uncompressed)
 *
 * Interface note:
 *   This file consumes the raw compressed bytes stored in the cache's
 *   segment storage (written big-endian by generate_entry / write_cache),
 *   plus the zero_bitmask stored separately in the tag entry.
 *
 *   zero_bitmask convention (matches compressor output):
 *     For N-element encodings, element i is stored at bit (N-1-i), i.e. MSB-first.
 *     - B8_D1, B8_D2, B8_D4: N=8,  element i at bit (7-i),  built with (0x80U >> i)
 *     - B4_D1, B4_D2:        N=16, element i at bit (15-i), built with (0x8000U >> i)
 *     - B2_D1:               N=32, element i at bit (31-i), built with (0x80000000U >> i)
 *
 *   zero_bitmask semantics (matches compressor):
 *     bit set (1) => element fits within delta range of zero base; use delta directly
 *     bit clear (0) => use base + delta
 *
 *   Compressed byte layout (big-endian, matches generate_entry output):
 *     [base: base_size bytes, BE] [delta_0: delta_size bytes, BE] ...
 *     COMP_TYPE_ZERO    : 1 byte  (zero token)
 *     COMP_TYPE_REP_VAL : 8 bytes (base only)
 *     COMP_TYPE_B8_D1   : 8 + 8*1  = 16 bytes
 *     COMP_TYPE_B8_D2   : 8 + 8*2  = 24 bytes
 *     COMP_TYPE_B8_D4   : 8 + 8*4  = 40 bytes
 *     COMP_TYPE_B4_D1   : 4 + 16*1 = 20 bytes
 *     COMP_TYPE_B4_D2   : 4 + 16*2 = 36 bytes
 *     COMP_TYPE_B2_D1   : 2 + 32*1 = 34 bytes
 */

#include <stdint.h>
#include <string.h>

#define CACHE_BLOCK_SIZE 64

/**
 * @brief Compression type enumeration (matches bdi_cache_test.c)
 */
typedef enum {
    COMP_TYPE_ZERO = 0,
    COMP_TYPE_REP_VAL,
    COMP_TYPE_B8_D1,
    COMP_TYPE_B8_D2,
    COMP_TYPE_B8_D4,
    COMP_TYPE_B4_D1,
    COMP_TYPE_B4_D2,
    COMP_TYPE_B2_D1,
    COMP_TYPE_NONE,
    NUM_COMP_TYPE
} comp_type_t;

/**
 * @brief Decompress a BDI cache line into 'out' (must be CACHE_BLOCK_SIZE bytes).
 *
 * Parameters:
 *   comp_type    - compression type/encoding (from the tag entry)
 *   zero_bitmask - per-element base selector bitmask (from the tag entry)
 *   cb           - pointer to the compressed bytes in segment storage (big-endian)
 *   out          - output buffer (CACHE_BLOCK_SIZE bytes)
 *
 * Returns 0 on success, -1 on unsupported comp_type.
 *
 * zero_bitmask bit indexing:
 *   The compressor sets bit (N-1-i) for element i (MSB-first ordering).
 *   Each case below samples the mask as: (zero_bitmask >> (N-1-i)) & 1
 *   where N is the number of elements for that encoding.
 */
int bdi_decompress(comp_type_t comp_type, int32_t zero_bitmask,
                   const uint8_t *cb, uint8_t out[CACHE_BLOCK_SIZE])
{
    switch (comp_type) {

    case COMP_TYPE_ZERO:
        memset(out, 0, CACHE_BLOCK_SIZE);
        return 0;

    case COMP_TYPE_REP_VAL: {
        /*
         * cb[0..7] holds the repeated 8-byte value, big-endian.
         * Cast to int64_t* and fill all 8 slots.
         */
        int64_t base = 0;
        for (int b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        int64_t *o = (int64_t *)out;
        for (int i = 0; i < 8; i++)
            o[i] = base;
        return 0;
    }

    case COMP_TYPE_B8_D1: {
        /*
         * cb[0..7]  : 8-byte base, big-endian.
         * cb[8..15] : eight 1-byte deltas.
         * N=8: element i is at bit (7-i) of zero_bitmask.
         * zero bit set => value is the delta itself (zero-base path).
         * zero bit clear => value is base + delta.
         */
        int64_t base = 0;
        for (int b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        int64_t *o = (int64_t *)out;
        for (int i = 0; i < 8; i++) {
            int8_t delta = (int8_t)cb[8 + i];
            o[i] = ((zero_bitmask >> (7 - i)) & 1) ? (int64_t)delta : base + delta;
        }
        return 0;
    }

    case COMP_TYPE_B8_D2: {
        /*
         * cb[0..7]   : 8-byte base, big-endian.
         * cb[8..23]  : eight 2-byte deltas, big-endian.
         * N=8: element i is at bit (7-i) of zero_bitmask.
         */
        int64_t base = 0;
        for (int b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        int64_t *o = (int64_t *)out;
        for (int i = 0; i < 8; i++) {
            int16_t delta = (int16_t)((cb[8 + i*2] << 8) | cb[8 + i*2 + 1]);
            o[i] = ((zero_bitmask >> (7 - i)) & 1) ? (int64_t)delta : base + delta;
        }
        return 0;
    }

    case COMP_TYPE_B8_D4: {
        /*
         * cb[0..7]   : 8-byte base, big-endian.
         * cb[8..39]  : eight 4-byte deltas, big-endian.
         * N=8: element i is at bit (7-i) of zero_bitmask.
         */
        int64_t base = 0;
        for (int b = 0; b < 8; b++)
            base = (base << 8) | cb[b];
        int64_t *o = (int64_t *)out;
        for (int i = 0; i < 8; i++) {
            int32_t delta = (int32_t)(  ((uint32_t)cb[8 + i*4]     << 24)
                                      | ((uint32_t)cb[8 + i*4 + 1] << 16)
                                      | ((uint32_t)cb[8 + i*4 + 2] <<  8)
                                      |  (uint32_t)cb[8 + i*4 + 3] );
            o[i] = ((zero_bitmask >> (7 - i)) & 1) ? (int64_t)delta : base + delta;
        }
        return 0;
    }

    case COMP_TYPE_B4_D1: {
        /*
         * cb[0..3]   : 4-byte base, big-endian.
         * cb[4..19]  : sixteen 1-byte deltas.
         * N=16: element i is at bit (15-i) of zero_bitmask.
         */
        int32_t base = (int32_t)(  ((uint32_t)cb[0] << 24)
                                 | ((uint32_t)cb[1] << 16)
                                 | ((uint32_t)cb[2] <<  8)
                                 |  (uint32_t)cb[3] );
        int32_t *o = (int32_t *)out;
        for (int i = 0; i < 16; i++) {
            int8_t delta = (int8_t)cb[4 + i];
            o[i] = ((zero_bitmask >> (15 - i)) & 1) ? (int32_t)delta : base + delta;
        }
        return 0;
    }

    case COMP_TYPE_B4_D2: {
        /*
         * cb[0..3]   : 4-byte base, big-endian.
         * cb[4..35]  : sixteen 2-byte deltas, big-endian.
         * N=16: element i is at bit (15-i) of zero_bitmask.
         */
        int32_t base = (int32_t)(  ((uint32_t)cb[0] << 24)
                                 | ((uint32_t)cb[1] << 16)
                                 | ((uint32_t)cb[2] <<  8)
                                 |  (uint32_t)cb[3] );
        int32_t *o = (int32_t *)out;
        for (int i = 0; i < 16; i++) {
            int16_t delta = (int16_t)((cb[4 + i*2] << 8) | cb[4 + i*2 + 1]);
            o[i] = ((zero_bitmask >> (15 - i)) & 1) ? (int32_t)delta : base + delta;
        }
        return 0;
    }

    case COMP_TYPE_B2_D1: {
        /*
         * cb[0..1]   : 2-byte base, big-endian.
         * cb[2..33]  : thirty-two 1-byte deltas.
         * N=32: element i is at bit (31-i) of zero_bitmask.
         * Cast zero_bitmask to uint32_t before shifting to avoid undefined
         * behaviour on right-shifting a negative int32.
         */
        int16_t base = (int16_t)((cb[0] << 8) | cb[1]);
        int16_t *o = (int16_t *)out;
        uint32_t zmask = (uint32_t)zero_bitmask;
        for (int i = 0; i < 32; i++) {
            int8_t delta = (int8_t)cb[2 + i];
            o[i] = ((zmask >> (31 - i)) & 1) ? (int16_t)delta : (int16_t)(base + delta);
        }
        return 0;
    }

    case COMP_TYPE_NONE:
        /*
         * Uncompressed: the original 64-byte block is stored verbatim.
         * The caller must handle COMP_TYPE_NONE separately (direct memcpy
         * from the segment storage). Return -1 to signal that.
         */
        return -1;

    default:
        return -1;
    }
}
