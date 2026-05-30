/* cache.c - cache module routines */

/* SimpleScalar(TM) Tool Suite
 * Copyright (C) 1994-2003 by Todd M. Austin, Ph.D. and SimpleScalar, LLC.
 * All Rights Reserved. 
 * 
 * THIS IS A LEGAL DOCUMENT, BY USING SIMPLESCALAR,
 * YOU ARE AGREEING TO THESE TERMS AND CONDITIONS.
 * 
 * No portion of this work may be used by any commercial entity, or for any
 * commercial purpose, without the prior, written permission of SimpleScalar,
 * LLC (info@simplescalar.com). Nonprofit and noncommercial use is permitted
 * as described below.
 * 
 * 1. SimpleScalar is provided AS IS, with no warranty of any kind, express
 * or implied. The user of the program accepts full responsibility for the
 * application of the program and the use of any results.
 * 
 * 2. Nonprofit and noncommercial use is encouraged. SimpleScalar may be
 * downloaded, compiled, executed, copied, and modified solely for nonprofit,
 * educational, noncommercial research, and noncommercial scholarship
 * purposes provided that this notice in its entirety accompanies all copies.
 * Copies of the modified software can be delivered to persons who use it
 * solely for nonprofit, educational, noncommercial research, and
 * noncommercial scholarship purposes provided that this notice in its
 * entirety accompanies all copies.
 * 
 * 3. ALL COMMERCIAL USE, AND ALL USE BY FOR PROFIT ENTITIES, IS EXPRESSLY
 * PROHIBITED WITHOUT A LICENSE FROM SIMPLESCALAR, LLC (info@simplescalar.com).
 * 
 * 4. No nonprofit user may place any restrictions on the use of this software,
 * including as modified by the user, by any other authorized user.
 * 
 * 5. Noncommercial and nonprofit users may distribute copies of SimpleScalar
 * in compiled or executable form as set forth in Section 2, provided that
 * either: (A) it is accompanied by the corresponding machine-readable source
 * code, or (B) it is accompanied by a written offer, with no time limit, to
 * give anyone a machine-readable copy of the corresponding source code in
 * return for reimbursement of the cost of distribution. This written offer
 * must permit verbatim duplication by anyone, or (C) it is distributed by
 * someone who received only the executable form, and is accompanied by a
 * copy of the written offer of source code.
 * 
 * 6. SimpleScalar was developed by Todd M. Austin, Ph.D. The tool suite is
 * currently maintained by SimpleScalar LLC (info@simplescalar.com). US Mail:
 * 2395 Timbercrest Court, Ann Arbor, MI 48105.
 * 
 * Copyright (C) 1994-2003 by Todd M. Austin, Ph.D. and SimpleScalar, LLC.
 */


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "host.h"
#include "misc.h"
#include "machine.h"
#include "cache.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>


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
            v |= ((uint64_t)data[i*8 + j]) << (8*j);
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
            v |= ((uint32_t)data[i*4 + j]) << (8 * j);
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
            v |= ((uint64_t)data[i*8 + j]) << (8 * j);
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
        uint16_t v = (uint16_t)data[i*2] |
            ((uint16_t)data[i*2 + 1] << 8);
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
            v |= ((uint32_t)data[i*4 + j]) << (8 * j);
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
            v |= ((uint64_t)data[i*8 + j]) << (8 * j);
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

/* cache access macros */
#define CACHE_TAG(cp, addr)	((addr) >> (cp)->tag_shift)
#define CACHE_SET(cp, addr)	(((addr) >> (cp)->set_shift) & (cp)->set_mask)
#define CACHE_BLK(cp, addr)	((addr) & (cp)->blk_mask)
#define CACHE_TAGSET(cp, addr)	((addr) & (cp)->tagset_mask)

/* extract/reconstruct a block address */
#define CACHE_BADDR(cp, addr)	((addr) & ~(cp)->blk_mask)
#define CACHE_MK_BADDR(cp, tag, set)					\
  (((tag) << (cp)->tag_shift)|((set) << (cp)->set_shift))

/* index an array of cache blocks, non-trivial due to variable length blocks */
#define CACHE_BINDEX(cp, blks, i)					\
  ((struct cache_blk_t *)(((char *)(blks)) +				\
			  (i)*(sizeof(struct cache_blk_t) +		\
			       ((cp)->balloc				\
				? (cp)->bsize*sizeof(byte_t) : 0))))

/* cache data block accessor, type parameterized */
#define __CACHE_ACCESS(type, data, bofs)				\
  (*((type *)(((char *)data) + (bofs))))

/* cache data block accessors, by type */
#define CACHE_DOUBLE(data, bofs)  __CACHE_ACCESS(double, data, bofs)
#define CACHE_FLOAT(data, bofs)	  __CACHE_ACCESS(float, data, bofs)
#define CACHE_WORD(data, bofs)	  __CACHE_ACCESS(unsigned int, data, bofs)
#define CACHE_HALF(data, bofs)	  __CACHE_ACCESS(unsigned short, data, bofs)
#define CACHE_BYTE(data, bofs)	  __CACHE_ACCESS(unsigned char, data, bofs)

/* cache block hashing macros, this macro is used to index into a cache
   set hash table (to find the correct block on N in an N-way cache), the
   cache set index function is CACHE_SET, defined above */
#define CACHE_HASH(cp, key)						\
  (((key >> 24) ^ (key >> 16) ^ (key >> 8) ^ key) & ((cp)->hsize-1))

/* copy data out of a cache block to buffer indicated by argument pointer p */
#define CACHE_BCOPY(cmd, blk, bofs, p, nbytes)	\
  if (cmd == Read)							\
    {									\
      switch (nbytes) {							\
      case 1:								\
	*((byte_t *)p) = CACHE_BYTE(&blk->data[0], bofs); break;	\
      case 2:								\
	*((half_t *)p) = CACHE_HALF(&blk->data[0], bofs); break;	\
      case 4:								\
	*((word_t *)p) = CACHE_WORD(&blk->data[0], bofs); break;	\
      default:								\
	{ /* >= 8, power of two, fits in block */			\
	  int words = nbytes >> 2;					\
	  while (words-- > 0)						\
	    {								\
	      *((word_t *)p) = CACHE_WORD(&blk->data[0], bofs);	\
	      p += 4; bofs += 4;					\
	    }\
	}\
      }\
    }\
  else /* cmd == Write */						\
    {									\
      switch (nbytes) {							\
      case 1:								\
	CACHE_BYTE(&blk->data[0], bofs) = *((byte_t *)p); break;	\
      case 2:								\
        CACHE_HALF(&blk->data[0], bofs) = *((half_t *)p); break;	\
      case 4:								\
	CACHE_WORD(&blk->data[0], bofs) = *((word_t *)p); break;	\
      default:								\
	{ /* >= 8, power of two, fits in block */			\
	  int words = nbytes >> 2;					\
	  while (words-- > 0)						\
	    {								\
	      CACHE_WORD(&blk->data[0], bofs) = *((word_t *)p);		\
	      p += 4; bofs += 4;					\
	    }\
	}\
    }\
  }

/* bound sqword_t/dfloat_t to positive int */
#define BOUND_POS(N)		((int)(MIN(MAX(0, (N)), 2147483647)))

typedef struct {
    uint8_t base;       // Base size (in bytes)
    uint8_t delta;      // Delta size (in bytes)
    uint8_t size_32;    // Compressed size for a 32-byte cache line (in bytes)
    uint8_t size_64;    // Compressed size for a 64-byte cache line (in bytes)
} comp_type_info_t;

/**
 * @brief Array of compression type info
 * 
 */
comp_type_info_t comp_types[] = {
    [BDI_ZERO]    = {.base=1, .delta=0, .size_32=1,  .size_64=1},
    [BDI_REP_VAL] = {.base=8, .delta=0, .size_32=8,  .size_64=8},
    [BDI_B8_D1]   = {.base=8, .delta=1, .size_32=12, .size_64=16},
    [BDI_B8_D2]   = {.base=8, .delta=2, .size_32=16, .size_64=24},
    [BDI_B8_D4]   = {.base=8, .delta=4, .size_32=24, .size_64=40},
    [BDI_B4_D1]   = {.base=4, .delta=1, .size_32=12, .size_64=20},
    [BDI_B4_D2]   = {.base=4, .delta=2, .size_32=20, .size_64=36},
    [BDI_B2_D1]   = {.base=2, .delta=1, .size_32=18, .size_64=34},
    [BDI_NONE]    = {.base=0, .delta=0, .size_32=32, .size_64=64},
};

/**
 * @brief Get the compression type's data size for a 64-byte cache block
 * 
 * @param comp_type 
 * @return uint8_t 
 */
static inline uint8_t get_comp_size_64(bdi_type_t comp_type) {return comp_types[comp_type].size_64;}

/**
 * @brief Get the segments required to hold the data for the given compression type (64-byte block mode)
 * 
 * @param comp_type 
 * @return uint8_t 
 */
static inline uint8_t get_segments_req_64(bdi_type_t comp_type) {return 1+((get_comp_size_64(comp_type)-1) / CACHE_SEGMENT_SIZE);}


/* unlink BLK from the hash table bucket chain in SET */
static void
unlink_htab_ent(struct cache_t *cp,		/* cache to update */
		struct cache_set_t *set,	/* set containing bkt chain */
		struct cache_blk_t *blk)	/* block to unlink */
{
  struct cache_blk_t *prev, *ent;
  int index = CACHE_HASH(cp, blk->tag);

  /* locate the block in the hash table bucket chain */
  for (prev=NULL,ent=set->hash[index];
       ent;
       prev=ent,ent=ent->hash_next)
    {
      if (ent == blk)
	break;
    }
  assert(ent);

  /* unlink the block from the hash table bucket chain */
  if (!prev)
    {
      /* head of hash bucket list */
      set->hash[index] = ent->hash_next;
    }
  else
    {
      /* middle or end of hash bucket list */
      prev->hash_next = ent->hash_next;
    }
  ent->hash_next = NULL;
}

/* insert BLK onto the head of the hash table bucket chain in SET */
static void
link_htab_ent(struct cache_t *cp,		/* cache to update */
	      struct cache_set_t *set,		/* set containing bkt chain */
	      struct cache_blk_t *blk)		/* block to insert */
{
  int index = CACHE_HASH(cp, blk->tag);

  /* insert block onto the head of the bucket chain */
  blk->hash_next = set->hash[index];
  set->hash[index] = blk;
}

/* where to insert a block onto the ordered way chain */
enum list_loc_t { Head, Tail };

/* insert BLK into the order way chain in SET at location WHERE */
static void
update_way_list(struct cache_set_t *set,	/* set contained way chain */
		struct cache_blk_t *blk,	/* block to insert */
		enum list_loc_t where)		/* insert location */
{
  /* unlink entry from the way list */
  if (!blk->way_prev && !blk->way_next)
    {
      /* only one entry in list (direct-mapped), no action */
      assert(set->way_head == blk && set->way_tail == blk);
      /* Head/Tail order already */
      return;
    }
  /* else, more than one element in the list */
  else if (!blk->way_prev)
    {
      assert(set->way_head == blk && set->way_tail != blk);
      if (where == Head)
	{
	  /* already there */
	  return;
	}
      /* else, move to tail */
      set->way_head = blk->way_next;
      blk->way_next->way_prev = NULL;
    }
  else if (!blk->way_next)
    {
      /* end of list (and not front of list) */
      assert(set->way_head != blk && set->way_tail == blk);
      if (where == Tail)
	{
	  /* already there */
	  return;
	}
      set->way_tail = blk->way_prev;
      blk->way_prev->way_next = NULL;
    }
  else
    {
      /* middle of list (and not front or end of list) */
      assert(set->way_head != blk && set->way_tail != blk);
      blk->way_prev->way_next = blk->way_next;
      blk->way_next->way_prev = blk->way_prev;
    }

  /* link BLK back into the list */
  if (where == Head)
    {
      /* link to the head of the way list */
      blk->way_next = set->way_head;
      blk->way_prev = NULL;
      set->way_head->way_prev = blk;
      set->way_head = blk;
    }
  else if (where == Tail)
    {
      /* link to the tail of the way list */
      blk->way_prev = set->way_tail;
      blk->way_next = NULL;
      set->way_tail->way_next = blk;
      set->way_tail = blk;
    }
  else
    panic("bogus WHERE designator");
}

/* create and initialize a general cache structure */
struct cache_t *			/* pointer to cache created */
cache_create(char *name,		/* name of the cache */
	     int nsets,			/* total number of sets in cache */
	     int bsize,			/* block (line) size of cache */
	     int balloc,		/* allocate data space for blocks? */
	     int usize,			/* size of user data to alloc w/blks */
	     int assoc,			/* associativity of cache */
	     enum cache_policy policy,	/* replacement policy w/in sets */
	     /* block access function, see description w/in struct cache def */
	     unsigned int (*blk_access_fn)(enum mem_cmd cmd,
					   md_addr_t baddr, int bsize,
					   struct cache_blk_t *blk,
					   tick_t now),
	     unsigned int hit_latency)	/* latency in cycles for a hit */
{
  struct cache_t *cp;
  struct cache_blk_t *blk;
  int i, j, bindex;

  /* check all cache parameters */
  if (nsets <= 0)
    fatal("cache size (in sets) `%d' must be non-zero", nsets);
  if ((nsets & (nsets-1)) != 0)
    fatal("cache size (in sets) `%d' is not a power of two", nsets);
  /* blocks must be at least one datum large, i.e., 8 bytes for SS */
  if (bsize < 8)
    fatal("cache block size (in bytes) `%d' must be 8 or greater", bsize);
  if ((bsize & (bsize-1)) != 0)
    fatal("cache block size (in bytes) `%d' must be a power of two", bsize);
  if (usize < 0)
    fatal("user data size (in bytes) `%d' must be a positive value", usize);
  if (assoc <= 0)
    fatal("cache associativity `%d' must be non-zero and positive", assoc);
  if ((assoc & (assoc-1)) != 0)
    fatal("cache associativity `%d' must be a power of two", assoc);
  if (!blk_access_fn)
    fatal("must specify miss/replacement functions");

  /* allocate the cache structure */
  cp = (struct cache_t *)
    calloc(1, sizeof(struct cache_t) + (nsets-1)*sizeof(struct cache_set_t));
  if (!cp)
    fatal("out of virtual memory");

  /* initialize user parameters */
  cp->name = mystrdup(name);
  cp->nsets = nsets;
  cp->bsize = bsize;
  cp->balloc = balloc;
  cp->usize = usize;
  cp->is_bdi = (policy == BDI_LRU) ? 1: 0;
  if(cp->is_bdi) cp->assoc = 2*assoc;
  else cp->assoc = assoc;
  cp->policy = policy;
  cp->hit_latency = hit_latency;

  /* miss/replacement functions */
  cp->blk_access_fn = blk_access_fn;

  /* compute derived parameters */
  cp->hsize = CACHE_HIGHLY_ASSOC(cp) ? (assoc >> 2) : 0;
  cp->blk_mask = bsize-1;
  cp->set_shift = log_base2(bsize);
  cp->set_mask = nsets-1;
  cp->tag_shift = cp->set_shift + log_base2(nsets);
  cp->tag_mask = (1 << (32 - cp->tag_shift))-1;
  cp->tagset_mask = ~cp->blk_mask;
  cp->bus_free = 0;

  /* print derived parameters during debug */
  debug("%s: cp->hsize     = %d", cp->name, cp->hsize);
  debug("%s: cp->blk_mask  = 0x%08x", cp->name, cp->blk_mask);
  debug("%s: cp->set_shift = %d", cp->name, cp->set_shift);
  debug("%s: cp->set_mask  = 0x%08x", cp->name, cp->set_mask);
  debug("%s: cp->tag_shift = %d", cp->name, cp->tag_shift);
  debug("%s: cp->tag_mask  = 0x%08x", cp->name, cp->tag_mask);

  /* initialize cache stats */
  cp->hits = 0;
  cp->misses = 0;
  cp->replacements = 0;
  cp->writebacks = 0;
  cp->invalidations = 0;

  /* blow away the last block accessed */
  cp->last_tagset = 0;
  cp->last_blk = NULL;

  /* allocate data blocks */
  cp->data = (byte_t *)calloc(nsets * cp->assoc,
			      sizeof(struct cache_blk_t) +
			      (cp->balloc ? (bsize*sizeof(byte_t)) : 0));
  if (!cp->data)
    fatal("out of virtual memory");

  /* slice up the data blocks */
  for (bindex=0,i=0; i<nsets; i++)
    {
      cp->sets[i].way_head = NULL;
      cp->sets[i].way_tail = NULL;
      /* get a hash table, if needed */
      if (cp->hsize)
	{
	  cp->sets[i].hash =
	    (struct cache_blk_t **)calloc(cp->hsize,
					  sizeof(struct cache_blk_t *));
	  if (!cp->sets[i].hash)
	    fatal("out of virtual memory");
	}
      /* NOTE: all the blocks in a set *must* be allocated contiguously,
	 otherwise, block accesses through SET->BLKS will fail (used
	 during random replacement selection) */
      cp->sets[i].blks = CACHE_BINDEX(cp, cp->data, bindex);
      
      /* link the data blocks into ordered way chain and hash table bucket
         chains, if hash table exists */
      for (j=0; j<cp->assoc; j++)
	{
	  /* locate next cache block */
	  blk = CACHE_BINDEX(cp, cp->data, bindex);
	  bindex++;

	  /* invalidate new cache block */
	  blk->status = 0;
	  blk->tag = 0;
	  blk->ready = 0;
    blk->segment = 0;
	  blk->user_data = (usize != 0
			    ? (byte_t *)calloc(usize, sizeof(byte_t)) : NULL);

	  /* insert cache block into set hash table */
	  if (cp->hsize)
	    link_htab_ent(cp, &cp->sets[i], blk);

	  /* insert into head of way list, order is arbitrary at this point */
	  blk->way_next = cp->sets[i].way_head;
	  blk->way_prev = NULL;
	  if (cp->sets[i].way_head)
	    cp->sets[i].way_head->way_prev = blk;
	  cp->sets[i].way_head = blk;
	  if (!cp->sets[i].way_tail)
	    cp->sets[i].way_tail = blk;
	}
    }
  return cp;
}

/* parse policy */
enum cache_policy			/* replacement policy enum */
cache_char2policy(char c)		/* replacement policy as a char */
{
  switch (c) {
  case 'l': return LRU;
  case 'r': return Random;
  case 'f': return FIFO;
  case 'b': return BDI_LRU;
  default: fatal("bogus replacement policy, `%c'", c);
  }
}

/* print cache configuration */
void
cache_config(struct cache_t *cp,	/* cache instance */
	     FILE *stream)		/* output stream */
{
  fprintf(stream,
	  "cache: %s: %d sets, %d byte blocks, %d bytes user data/block\n",
	  cp->name, cp->nsets, cp->bsize, cp->usize);
  fprintf(stream,
	  "cache: %s: %d-way, `%s' replacement policy, write-back\n",
	  cp->name, cp->assoc,
	  cp->policy == LRU ? "LRU"
	  : cp->policy == Random ? "Random"
	  : cp->policy == FIFO ? "FIFO"
    : cp->policy == BDI_LRU ? "BDI LRU"
	  : (abort(), ""));
}

/* register cache stats */
void
cache_reg_stats(struct cache_t *cp,	/* cache instance */
		struct stat_sdb_t *sdb)	/* stats database */
{
  char buf[512], buf1[512], *name;

  /* get a name for this cache */
  if (!cp->name || !cp->name[0])
    name = "<unknown>";
  else
    name = cp->name;

  sprintf(buf, "%s.accesses", name);
  sprintf(buf1, "%s.hits + %s.misses", name, name);
  stat_reg_formula(sdb, buf, "total number of accesses", buf1, "%12.0f");
  sprintf(buf, "%s.hits", name);
  stat_reg_counter(sdb, buf, "total number of hits", &cp->hits, 0, NULL);
  sprintf(buf, "%s.misses", name);
  stat_reg_counter(sdb, buf, "total number of misses", &cp->misses, 0, NULL);
  sprintf(buf, "%s.replacements", name);
  stat_reg_counter(sdb, buf, "total number of replacements",
		 &cp->replacements, 0, NULL);
  sprintf(buf, "%s.writebacks", name);
  stat_reg_counter(sdb, buf, "total number of writebacks",
		 &cp->writebacks, 0, NULL);
  sprintf(buf, "%s.invalidations", name);
  stat_reg_counter(sdb, buf, "total number of invalidations",
		 &cp->invalidations, 0, NULL);
  sprintf(buf, "%s.miss_rate", name);
  sprintf(buf1, "%s.misses / %s.accesses", name, name);
  stat_reg_formula(sdb, buf, "miss rate (i.e., misses/ref)", buf1, NULL);
  sprintf(buf, "%s.repl_rate", name);
  sprintf(buf1, "%s.replacements / %s.accesses", name, name);
  stat_reg_formula(sdb, buf, "replacement rate (i.e., repls/ref)", buf1, NULL);
  sprintf(buf, "%s.wb_rate", name);
  sprintf(buf1, "%s.writebacks / %s.accesses", name, name);
  stat_reg_formula(sdb, buf, "writeback rate (i.e., wrbks/ref)", buf1, NULL);
  sprintf(buf, "%s.inv_rate", name);
  sprintf(buf1, "%s.invalidations / %s.accesses", name, name);
  stat_reg_formula(sdb, buf, "invalidation rate (i.e., invs/ref)", buf1, NULL);

  /* BDI compression statistics (only meaningful when bsize == 64) */
  if(cp->is_bdi){
    sprintf(buf, "%s.bdi_total_fills", name);
    stat_reg_counter(sdb, buf, "BDI: total block fills analysed",
         &cp->bdi_total_fills, 0, NULL);
    sprintf(buf, "%s.bdi_comp_fills", name);
    stat_reg_counter(sdb, buf, "BDI: fills that were compressible",
         &cp->bdi_comp_fills, 0, NULL);
    sprintf(buf, "%s.bdi_comp_rate", name);
    sprintf(buf1, "%s.bdi_comp_fills / %s.bdi_total_fills", name, name);
    stat_reg_formula(sdb, buf, "BDI: fraction of fills that compress",
         buf1, NULL);
    sprintf(buf, "%s.bdi_bytes_raw", name);
    stat_reg_counter(sdb, buf, "BDI: total raw bytes across all fills",
         &cp->bdi_bytes_raw, 0, NULL);
    sprintf(buf, "%s.bdi_bytes_compressed", name);
    stat_reg_counter(sdb, buf, "BDI: total compressed bytes across all fills",
         &cp->bdi_bytes_compressed, 0, NULL);
    sprintf(buf, "%s.bdi_compress_ratio", name);
    sprintf(buf1, "%s.bdi_bytes_raw / %s.bdi_bytes_compressed", name, name);
    stat_reg_formula(sdb, buf, "BDI: overall compression ratio (raw/compressed)",
         buf1, NULL);
  
    /* per-encoding breakdown */
    { int t;
      for (t = 0; t < BDI_NUM_TYPES; t++) {
        sprintf(buf, "%s.bdi_%s", name, bdi_type_names[t]);
        sprintf(buf1, "BDI: fills encoded as %s", bdi_type_names[t]);
        stat_reg_counter(sdb, buf, buf1,
             &cp->bdi_type_count[t], 0, NULL);
      }
    }
  }
}

/* print cache stats */
void
cache_stats(struct cache_t *cp,		/* cache instance */
	    FILE *stream)		/* output stream */
{
  double sum = (double)(cp->hits + cp->misses);

  fprintf(stream,
	  "cache: %s: %.0f hits %.0f misses %.0f repls %.0f invalidations\n",
	  cp->name, (double)cp->hits, (double)cp->misses,
	  (double)cp->replacements, (double)cp->invalidations);
  fprintf(stream,
	  "cache: %s: miss rate=%f  repl rate=%f  invalidation rate=%f\n",
	  cp->name,
	  (double)cp->misses/sum, (double)(double)cp->replacements/sum,
	  (double)cp->invalidations/sum);
}

/* access a cache, perform a CMD operation on cache CP at address ADDR,
   places NBYTES of data at *P, returns latency of operation if initiated
   at NOW, places pointer to block user data in *UDATA, *P is untouched if
   cache blocks are not allocated (!CP->BALLOC), UDATA should be NULL if no
   user data is attached to blocks */
unsigned int				/* latency of access in cycles */
cache_access(struct cache_t *cp,	/* cache to access */
	     enum mem_cmd cmd,		/* access type, Read or Write */
	     md_addr_t addr,		/* address of access */
	     void *vp,			/* ptr to buffer for input/output */
	     int nbytes,		/* number of bytes to access */
	     tick_t now,		/* time of access */
	     byte_t **udata,		/* for return of user data ptr */
	     md_addr_t *repl_addr)	/* for address of replaced block */
{
  byte_t *p = vp;
  md_addr_t tag = CACHE_TAG(cp, addr);
  md_addr_t set = CACHE_SET(cp, addr);
  md_addr_t bofs = CACHE_BLK(cp, addr);
  struct cache_blk_t *blk, *repl;
  int lat = 0;
  int32_t zmask = 0;
  bdi_type_t btype;
  uint8_t required_segments = 0;
  int max_segments = (cp->assoc/2) * (cp->bsize/8);
  int available_tags = 0;
  struct cache_blk_t *temp_blk = NULL;
  int comp_size = 0;

  if(cp->is_bdi){
    temp_blk = calloc(1, sizeof(struct cache_blk_t) + ((cp->bsize-1)*sizeof(byte_t)));
    temp_blk->status = 0;
    temp_blk->tag = 0;
  }

  /* default replacement address */
  if (repl_addr)
    *repl_addr = 0;

  /* check alignments */
  if ((nbytes & (nbytes-1)) != 0 || (addr & (nbytes-1)) != 0)
    fatal("cache: access error: bad size or alignment, addr 0x%08x", addr);

  /* access must fit in cache block */
  /* FIXME:
     ((addr + (nbytes - 1)) > ((addr & ~cp->blk_mask) + (cp->bsize - 1))) */
  if ((addr + nbytes) > ((addr & ~cp->blk_mask) + cp->bsize))
    fatal("cache: access error: access spans block, addr 0x%08x", addr);

  /* permissions are checked on cache misses */

  /* check for a fast hit: access to same block */
  if (CACHE_TAGSET(cp, addr) == cp->last_tagset)
    {
      /* hit in the same block */
      blk = cp->last_blk;
      goto cache_fast_hit;
    }
    
  if (cp->hsize)
    {
      /* higly-associativity cache, access through the per-set hash tables */
      int hindex = CACHE_HASH(cp, tag);

      for (blk=cp->sets[set].hash[hindex];
	   blk;
	   blk=blk->hash_next)
	{
	  if (blk->tag == tag && (blk->status & CACHE_BLK_VALID))
	    goto cache_hit;
	}
    }
  else
    {
      /* low-associativity cache, linear search the way list */
      for (blk=cp->sets[set].way_head;
	   blk;
	   blk=blk->way_next)
	{
	  if (blk->tag == tag && (blk->status & CACHE_BLK_VALID))
	    goto cache_hit;
	}
    }

  /* cache block not found */

  /* **MISS** */
  cp->misses++;
  if(cp->is_bdi){
    lat += cp->blk_access_fn(Read, CACHE_BADDR(cp, addr), cp->bsize,
           temp_blk, now+lat);
    btype     = bdi_compress(temp_blk->data, &zmask);
    comp_size = bdi_compressed_size(btype);
    required_segments = get_segments_req_64(btype);
  }
  /* select the appropriate block to replace, and re-link this entry to
     the appropriate place in the way list */
  switch (cp->policy) {
  case LRU:
  case FIFO:
    repl = cp->sets[set].way_tail;
    update_way_list(&cp->sets[set], repl, Head);
    break;
  case Random:
    {
      int bindex = myrand() & (cp->assoc - 1);
      repl = CACHE_BINDEX(cp, cp->sets[set].blks, bindex);
    }
    break;
  case BDI_LRU:
    if(cp->is_bdi){
      
      for (blk=cp->sets[set].way_head; blk; blk=blk->way_next) {
        if (!(blk->status & CACHE_BLK_VALID)){
          available_tags++;
        }
      }
      if(available_tags == 0){
        /* write back replaced block data */
        if (cp->sets[set].way_tail->status & CACHE_BLK_VALID) {
          cp->replacements++;
  
          // if (repl_addr) *repl_addr = CACHE_MK_BADDR(cp, repl->tag, set);
      
          /* don't replace the block until outstanding misses are satisfied */
          lat += BOUND_POS(cp->sets[set].way_tail->ready - now);
      
          /* stall until the bus to next level of memory is available */
          lat += BOUND_POS(cp->bus_free - (now + lat));
      
            /* track bus resource usage */
          cp->bus_free = MAX(cp->bus_free, (now + lat)) + 1;
  
          if (cp->sets[set].way_tail->status & CACHE_BLK_DIRTY){
            /* write back the cache block */
            cp->writebacks++;
            lat += cp->blk_access_fn(Write,
                  CACHE_MK_BADDR(cp, cp->sets[set].way_tail->tag, set),
                  cp->bsize, cp->sets[set].way_tail, now+lat);
          }
        }
        cp->sets[set].way_tail->status &= ~CACHE_BLK_VALID;
        cp->sets[set].used_segments -= cp->sets[set].way_tail->num_segments;
        cp->sets[set].way_tail->num_segments = 0;
      }
      for(blk=cp->sets[set].way_tail; blk; blk=blk->way_prev){
        if(cp->sets[set].used_segments + required_segments > max_segments){
          if((blk->status & CACHE_BLK_VALID)){
            cp->replacements++;
  
            // if (repl_addr) *repl_addr = CACHE_MK_BADDR(cp, repl->tag, set);
        
            /* don't replace the block until outstanding misses are satisfied */
            lat += BOUND_POS(blk->ready - now);
        
            /* stall until the bus to next level of memory is available */
            lat += BOUND_POS(cp->bus_free - (now + lat));
        
              /* track bus resource usage */
            cp->bus_free = MAX(cp->bus_free, (now + lat)) + 1;
  
            if (blk->status & CACHE_BLK_DIRTY){
              /* write back the cache block */
              cp->writebacks++;
              lat += cp->blk_access_fn(Write,
                    CACHE_MK_BADDR(cp, blk->tag, set),
                    cp->bsize, blk, now+lat);
            }
            blk->status &= ~CACHE_BLK_VALID;
            cp->sets[set].used_segments -= blk->num_segments;
            blk->num_segments = 0;
          }
        }
        else{
          break;
        }
      }
      for (blk=cp->sets[set].way_head; blk; blk=blk->way_next) {
        if (!(blk->status & CACHE_BLK_VALID)){
          repl = blk;
          break;
        }
      }
      if (repl == NULL) panic("BDI_LRU: Failed to find an invalid block for replacement!");
      update_way_list(&cp->sets[set], repl, Head);
    }
    break;
  default:
    panic("bogus replacement policy");
  }

  if(cp->is_bdi){
    assert(repl);
    assert(temp_blk);
    memcpy(repl->data, temp_blk->data, cp->bsize);
    free(temp_blk);
    repl->num_segments = required_segments;
    cp->sets[set].used_segments += required_segments;
  }

  /* remove this block from the hash bucket chain, if hash exists */
  if (cp->hsize)
    unlink_htab_ent(cp, &cp->sets[set], repl);
  
  /* blow away the last block to hit */
  cp->last_tagset = 0;
  cp->last_blk = NULL;
  
  /* write back replaced block data */
  // if(!cp->is_bdi){
    if (repl->status & CACHE_BLK_VALID)
      {
        cp->replacements++;
  
        if (repl_addr)
    *repl_addr = CACHE_MK_BADDR(cp, repl->tag, set);
   
        /* don't replace the block until outstanding misses are satisfied */
        lat += BOUND_POS(repl->ready - now);
   
        /* stall until the bus to next level of memory is available */
        lat += BOUND_POS(cp->bus_free - (now + lat));
   
        /* track bus resource usage */
        cp->bus_free = MAX(cp->bus_free, (now + lat)) + 1;
  
        if (repl->status & CACHE_BLK_DIRTY)
    {
      /* write back the cache block */
      cp->writebacks++;
      lat += cp->blk_access_fn(Write,
             CACHE_MK_BADDR(cp, repl->tag, set),
             cp->bsize, repl, now+lat);
    }
    }
  // }

  /* update block tags */
  repl->tag = tag;
  repl->status = CACHE_BLK_VALID;	/* dirty bit set on update */

  /* read data block */
  if(!cp->is_bdi){
    lat += cp->blk_access_fn(Read, CACHE_BADDR(cp, addr), cp->bsize,
           repl, now+lat);
  }

  /* BDI compression analysis: run on every fill for 64-byte blocks.
     We don't physically repack the data; we just record which encoding
     would apply and accumulate byte-savings statistics. */
  if (cp->is_bdi) {
      // int32_t zmask = 0;
      // bdi_type_t btype = bdi_compress((const uint8_t *)&repl->data[0], &zmask);
      // int comp_size    = bdi_compressed_size(btype);

      repl->bdi_type          = btype;
      repl->bdi_zero_bitmask  = zmask;

      cp->bdi_total_fills++;
      cp->bdi_bytes_raw        += BDI_BLOCK_SIZE;
      cp->bdi_bytes_compressed += comp_size;
      cp->bdi_type_count[btype]++;
      if (btype != BDI_NONE) cp->bdi_comp_fills++;
  }

  /* copy data out of cache block */
  if (cp->balloc)
    {
      if(p)
      CACHE_BCOPY(cmd, repl, bofs, p, nbytes);
    }

  /* update dirty status */
  if (cmd == Write)
    repl->status |= CACHE_BLK_DIRTY;

  /* get user block data, if requested and it exists */
  if (udata)
    *udata = repl->user_data;

  /* update block status */
  repl->ready = now+lat;

  /* link this entry back into the hash table */
  if (cp->hsize)
    link_htab_ent(cp, &cp->sets[set], repl);

  /* return latency of the operation */
  return lat;


 cache_hit: /* slow hit handler */
  
  /* **HIT** */
  cp->hits++;

  /* copy data out of cache block, if block exists */
  if (cp->balloc)
    {
      if(p)
      CACHE_BCOPY(cmd, blk, bofs, p, nbytes);
    }

  /* update dirty status */
  if (cmd == Write){
    blk->status |= CACHE_BLK_DIRTY;
    if(cp->is_bdi){
      cp->blk_access_fn(Read, CACHE_BADDR(cp, addr), cp->bsize, temp_blk, now+lat);
      btype = bdi_compress(temp_blk->data, &zmask);
      comp_size = bdi_compressed_size(btype);
      required_segments = get_segments_req_64(btype);
      int max_segments = (cp->assoc/2) * (cp->bsize/8);
      int segments_diff = required_segments - blk->num_segments;
      if(segments_diff > 0 && (cp->sets[set].used_segments + segments_diff > max_segments)){
        struct cache_blk_t *t_blk;
        for(t_blk=cp->sets[set].way_tail; t_blk; t_blk=t_blk->way_prev){
          if(t_blk == blk) continue;
          if(cp->sets[set].used_segments + segments_diff > max_segments){
            if((t_blk->status & CACHE_BLK_VALID)){
              cp->replacements++;
  
              // if (repl_addr) *repl_addr = CACHE_MK_BADDR(cp, repl->tag, set);
          
              /* don't replace the block until outstanding misses are satisfied */
              lat += BOUND_POS(t_blk->ready - now);
          
              /* stall until the bus to next level of memory is available */
              lat += BOUND_POS(cp->bus_free - (now + lat));
          
                /* track bus resource usage */
              cp->bus_free = MAX(cp->bus_free, (now + lat)) + 1;
  
              if (t_blk->status & CACHE_BLK_DIRTY){
                /* write back the cache block */
                cp->writebacks++;
                lat += cp->blk_access_fn(Write,
                      CACHE_MK_BADDR(cp, t_blk->tag, set),
                      cp->bsize, t_blk, now+lat);
              }
              t_blk->status &= ~CACHE_BLK_VALID;
              cp->sets[set].used_segments -= t_blk->num_segments;
              t_blk->num_segments = 0;
            }
          }
          else{
            break;
          }
        }
      }
      blk->num_segments = required_segments;
      cp->sets[set].used_segments += segments_diff;
      memcpy(blk->data, temp_blk->data, cp->bsize);
      free(temp_blk);
    }
  }

  /* if LRU replacement and this is not the first element of list, reorder */
  if (blk->way_prev && (cp->policy == LRU || cp->policy == BDI_LRU))
    {
      /* move this block to head of the way (MRU) list */
      update_way_list(&cp->sets[set], blk, Head);
    }

  /* tag is unchanged, so hash links (if they exist) are still valid */

  /* record the last block to hit */
  cp->last_tagset = CACHE_TAGSET(cp, addr);
  cp->last_blk = blk;

  /* get user block data, if requested and it exists */
  if (udata)
    *udata = blk->user_data;

  /* return first cycle data is available to access */
  return (int) MAX(cp->hit_latency, (blk->ready - now));

 cache_fast_hit: /* fast hit handler */
  
  /* **FAST HIT** */
  cp->hits++;

  /* copy data out of cache block, if block exists */
  if (cp->balloc)
    { 
      if(p)
      CACHE_BCOPY(cmd, blk, bofs, p, nbytes);
    }

  /* update dirty status */
  if (cmd == Write){
    blk->status |= CACHE_BLK_DIRTY;
    if(cp->is_bdi){
      cp->blk_access_fn(Read, CACHE_BADDR(cp, addr), cp->bsize, temp_blk, now+lat);
      btype = bdi_compress(temp_blk->data, &zmask);
      comp_size = bdi_compressed_size(btype);
      required_segments = get_segments_req_64(btype);
      int max_segments = (cp->assoc/2) * (cp->bsize/8);
      int segments_diff = required_segments - blk->num_segments;
      if(segments_diff > 0 && (cp->sets[set].used_segments + segments_diff > max_segments)){
        struct cache_blk_t *t_blk;
        for(t_blk=cp->sets[set].way_tail; t_blk; t_blk=t_blk->way_prev){
          if(t_blk == blk) continue;
          if(cp->sets[set].used_segments + segments_diff > max_segments){
            if((t_blk->status & CACHE_BLK_VALID)){
              cp->replacements++;
  
              // if (repl_addr) *repl_addr = CACHE_MK_BADDR(cp, repl->tag, set);
          
              /* don't replace the block until outstanding misses are satisfied */
              lat += BOUND_POS(t_blk->ready - now);
          
              /* stall until the bus to next level of memory is available */
              lat += BOUND_POS(cp->bus_free - (now + lat));
          
                /* track bus resource usage */
              cp->bus_free = MAX(cp->bus_free, (now + lat)) + 1;
  
              if (t_blk->status & CACHE_BLK_DIRTY){
                /* write back the cache block */
                cp->writebacks++;
                lat += cp->blk_access_fn(Write,
                      CACHE_MK_BADDR(cp, t_blk->tag, set),
                      cp->bsize, t_blk, now+lat);
              }
              t_blk->status &= ~CACHE_BLK_VALID;
              cp->sets[set].used_segments -= t_blk->num_segments;
              t_blk->num_segments = 0;
            }
          }
          else{
            break;
          }
        }
      }
      blk->num_segments = required_segments;
      cp->sets[set].used_segments += segments_diff;
      memcpy(blk->data, temp_blk->data, cp->bsize);
      free(temp_blk);
    }
  }

  /* this block hit last, no change in the way list */

  /* tag is unchanged, so hash links (if they exist) are still valid */

  /* get user block data, if requested and it exists */
  if (udata)
    *udata = blk->user_data;

  /* record the last block to hit */
  cp->last_tagset = CACHE_TAGSET(cp, addr);
  cp->last_blk = blk;

  /* return first cycle data is available to access */
  return (int) MAX(cp->hit_latency, (blk->ready - now));
}

/* return non-zero if block containing address ADDR is contained in cache
   CP, this interface is used primarily for debugging and asserting cache
   invariants */
int					/* non-zero if access would hit */
cache_probe(struct cache_t *cp,		/* cache instance to probe */
	    md_addr_t addr)		/* address of block to probe */
{
  md_addr_t tag = CACHE_TAG(cp, addr);
  md_addr_t set = CACHE_SET(cp, addr);
  struct cache_blk_t *blk;

  /* permissions are checked on cache misses */

  if (cp->hsize)
  {
    /* higly-associativity cache, access through the per-set hash tables */
    int hindex = CACHE_HASH(cp, tag);
    
    for (blk=cp->sets[set].hash[hindex];
	 blk;
	 blk=blk->hash_next)
    {	
      if (blk->tag == tag && (blk->status & CACHE_BLK_VALID))
	  return TRUE;
    }
  }
  else
  {
    /* low-associativity cache, linear search the way list */
    for (blk=cp->sets[set].way_head;
	 blk;
	 blk=blk->way_next)
    {
      if (blk->tag == tag && (blk->status & CACHE_BLK_VALID))
	  return TRUE;
    }
  }
  
  /* cache block not found */
  return FALSE;
}

/* flush the entire cache, returns latency of the operation */
unsigned int				/* latency of the flush operation */
cache_flush(struct cache_t *cp,		/* cache instance to flush */
	    tick_t now)			/* time of cache flush */
{
  int i, lat = cp->hit_latency; /* min latency to probe cache */
  struct cache_blk_t *blk;

  /* blow away the last block to hit */
  cp->last_tagset = 0;
  cp->last_blk = NULL;

  /* no way list updates required because all blocks are being invalidated */
  for (i=0; i<cp->nsets; i++)
    {
      cp->sets[i].used_segments = 0;
      for (blk=cp->sets[i].way_head; blk; blk=blk->way_next)
	{
	  if (blk->status & CACHE_BLK_VALID)
	    {
	      cp->invalidations++;
	      blk->status &= ~CACHE_BLK_VALID;

	      if (blk->status & CACHE_BLK_DIRTY)
		{
		  /* write back the invalidated block */
          	  cp->writebacks++;
		  lat += cp->blk_access_fn(Write,
					   CACHE_MK_BADDR(cp, blk->tag, i),
					   cp->bsize, blk, now+lat);
		}
	    }
	}
    }

  /* return latency of the flush operation */
  return lat;
}

/* flush the block containing ADDR from the cache CP, returns the latency of
   the block flush operation */
unsigned int				/* latency of flush operation */
cache_flush_addr(struct cache_t *cp,	/* cache instance to flush */
		 md_addr_t addr,	/* address of block to flush */
		 tick_t now)		/* time of cache flush */
{
  md_addr_t tag = CACHE_TAG(cp, addr);
  md_addr_t set = CACHE_SET(cp, addr);
  struct cache_blk_t *blk;
  int lat = cp->hit_latency; /* min latency to probe cache */

  if (cp->hsize)
    {
      /* higly-associativity cache, access through the per-set hash tables */
      int hindex = CACHE_HASH(cp, tag);

      for (blk=cp->sets[set].hash[hindex];
	   blk;
	   blk=blk->hash_next)
	{
	  if (blk->tag == tag && (blk->status & CACHE_BLK_VALID))
	    break;
	}
    }
  else
    {
      /* low-associativity cache, linear search the way list */
      for (blk=cp->sets[set].way_head;
	   blk;
	   blk=blk->way_next)
	{
	  if (blk->tag == tag && (blk->status & CACHE_BLK_VALID))
	    break;
	}
    }

  if (blk)
    {
      cp->invalidations++;
      blk->status &= ~CACHE_BLK_VALID;
      cp->sets[set].used_segments -= blk->num_segments; 
      blk->num_segments = 0;
      /* blow away the last block to hit */
      cp->last_tagset = 0;
      cp->last_blk = NULL;

      if (blk->status & CACHE_BLK_DIRTY)
	{
	  /* write back the invalidated block */
          cp->writebacks++;
	  lat += cp->blk_access_fn(Write,
				   CACHE_MK_BADDR(cp, blk->tag, set),
				   cp->bsize, blk, now+lat);
	}
      /* move this block to tail of the way (LRU) list */
      update_way_list(&cp->sets[set], blk, Tail);
    }

  /* return latency of the operation */
  return lat;
}
