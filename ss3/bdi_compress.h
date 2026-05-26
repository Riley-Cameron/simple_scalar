/* bdi_compress.h - Base-Delta-Immediate (BDI) cache compression interface */

/*
 * BDI compression as described in:
 *   Pekhimenko et al., "Base-Delta-Immediate Compression: Practical Data
 *   Compression for On-Chip Caches", PACT 2012.
 *
 * This module is used by cache.c to track compression metadata on every
 * L2 cache block fill (64-byte blocks only).  It does NOT physically
 * reorganise SimpleScalar's segment storage; instead it records which BDI
 * encoding would apply and accumulates byte-savings statistics that are
 * reported alongside the standard cache counters at the end of simulation.
 *
 * Original POC by Mohammed (poc/bdi_cache_test.c, poc/bdi_decompression.c).
 * Adapted for SimpleScalar integration.
 */

#ifndef BDI_COMPRESS_H
#define BDI_COMPRESS_H

#include <stdint.h>

/* BDI only applies to 64-byte cache blocks */
#define BDI_BLOCK_SIZE 64

/*
 * BDI encoding types, listed in priority order (best compression first).
 * The compressor tries each in sequence and returns the first that fits.
 *
 * Compressed sizes for a 64-byte source block:
 *   BDI_ZERO      ->  1 byte   (all-zero token)
 *   BDI_REP_VAL   ->  8 bytes  (one repeated 8-byte value)
 *   BDI_B8_D1     -> 16 bytes  (8-byte base + 8 x 1-byte deltas)
 *   BDI_B4_D1     -> 20 bytes  (4-byte base + 16 x 1-byte deltas)
 *   BDI_B8_D2     -> 24 bytes  (8-byte base + 8 x 2-byte deltas)
 *   BDI_B2_D1     -> 34 bytes  (2-byte base + 32 x 1-byte deltas)
 *   BDI_B4_D2     -> 36 bytes  (4-byte base + 16 x 2-byte deltas)
 *   BDI_B8_D4     -> 40 bytes  (8-byte base + 8 x 4-byte deltas)
 *   BDI_NONE      -> 64 bytes  (uncompressible, stored raw)
 */
typedef enum {
    BDI_ZERO    = 0,
    BDI_REP_VAL,
    BDI_B8_D1,
    BDI_B4_D1,
    BDI_B8_D2,
    BDI_B2_D1,
    BDI_B4_D2,
    BDI_B8_D4,
    BDI_NONE,
    BDI_NUM_TYPES
} bdi_type_t;

/* Human-readable names (indexed by bdi_type_t) */
extern const char *bdi_type_names[BDI_NUM_TYPES];

/*
 * bdi_compressed_size - return the compressed byte count for a 64-byte block
 *   encoded with TYPE.  Returns 64 for unknown types.
 */
int bdi_compressed_size(bdi_type_t type);

/*
 * bdi_compress - analyse a BDI_BLOCK_SIZE (64-byte) block and return the
 *   best-fitting BDI encoding type.
 *
 *   data            - pointer to the 64-byte source block
 *   zero_bitmask_out - if non-NULL, receives the per-element zero-base
 *                      selector bitmask produced by the winning encoder
 *                      (0 for ZERO / REP_VAL / NONE where it is unused)
 *
 *   Returns BDI_NONE if the block cannot be compressed.
 */
bdi_type_t bdi_compress(const uint8_t *data, int32_t *zero_bitmask_out);

#endif /* BDI_COMPRESS_H */
