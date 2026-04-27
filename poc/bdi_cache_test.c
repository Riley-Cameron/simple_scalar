#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define LOG2_8(x)  ((x) >= 128 ? 7 : (x) >= 64 ? 6 : (x) >= 32 ? 5 : (x) >= 16 ? 4 : \
                    (x) >= 8   ? 3 : (x) >= 4  ? 2 : (x) >= 2  ? 1 : 0)
#define LOG2_16(x) ((x) >= 32768 ? 15 : (x) >= 256 ? 8 + LOG2_8((x) >> 8) : LOG2_8(x))
#define LOG2_32(x) ((x) >= 65536 ? 16 + LOG2_16((x) >> 16) : LOG2_16(x))


#define CACHE_BLOCK_SIZE    64
#define CACHE_WAYS          4
#define CACHE_SETS          1024
#define CACHE_SEGMENT_SIZE  8
#define CACHE_OFFSET_BITS   LOG2_32(CACHE_BLOCK_SIZE)
#define CACHE_INDEX_BITS    LOG2_32(CACHE_SETS)
#define CACHE_TAG_BITS      (32-CACHE_OFFSET_BITS-CACHE_INDEX_BITS)
#define CACHE_TAGS          (CACHE_WAYS*2)
#define CACHE_SET_SIZE      (CACHE_WAYS*CACHE_BLOCK_SIZE)
#define CACHE_SEGMENTS      (CACHE_SET_SIZE/CACHE_SEGMENT_SIZE)
#define CACHE_SEGMENT_BITS  (LOG2_8(CACHE_SEGMENTS))
#define CACHE_LRU_BITS      (LOG2_8(CACHE_TAGS))

/**
 * @brief Compression type enumeration
 * 
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
 * @brief Struct to store information about each compression type
 * 
 */
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
    [COMP_TYPE_ZERO]    = {.base=1, .delta=0, .size_32=1,  .size_64=1},
    [COMP_TYPE_REP_VAL] = {.base=8, .delta=0, .size_32=8,  .size_64=8},
    [COMP_TYPE_B8_D1]   = {.base=8, .delta=1, .size_32=12, .size_64=16},
    [COMP_TYPE_B8_D2]   = {.base=8, .delta=2, .size_32=16, .size_64=24},
    [COMP_TYPE_B8_D4]   = {.base=8, .delta=4, .size_32=24, .size_64=40},
    [COMP_TYPE_B4_D1]   = {.base=4, .delta=1, .size_32=12, .size_64=20},
    [COMP_TYPE_B4_D2]   = {.base=4, .delta=2, .size_32=20, .size_64=36},
    [COMP_TYPE_B2_D1]   = {.base=2, .delta=1, .size_32=18, .size_64=34},
    [COMP_TYPE_NONE]    = {.base=0, .delta=0, .size_32=32, .size_64=64},
};

typedef struct {
    uint32_t offset:CACHE_OFFSET_BITS;
    uint32_t index:CACHE_INDEX_BITS;
    uint32_t tag:CACHE_TAG_BITS;
} cache_addr_t;

typedef struct {
    uint64_t tag:CACHE_TAG_BITS;
    uint64_t valid:1;
    uint64_t segment:CACHE_SEGMENT_BITS;
    uint64_t lru:CACHE_LRU_BITS;
    comp_type_t comp_type;
} comp_cache_tag_t;

typedef struct {
    comp_cache_tag_t tags[CACHE_TAGS];
    uint8_t data[CACHE_SET_SIZE];
} comp_cache_set_t;

comp_cache_set_t BDI_CACHE[CACHE_SETS];

static inline uint8_t get_comp_base(comp_type_t comp_type) {return comp_types[comp_type].base;}
static inline uint8_t get_comp_delta(comp_type_t comp_type) {return comp_types[comp_type].delta;}
static inline uint8_t get_comp_size_32(comp_type_t comp_type) {return comp_types[comp_type].size_32;}
static inline uint8_t get_comp_size_64(comp_type_t comp_type) {return comp_types[comp_type].size_64;}

void print_data(uint8_t *data, uint8_t size) {
    for (int i = 0; i < size; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

int read_cache(uint32_t addr) {
    cache_addr_t *c_addr = (cache_addr_t *)&addr;
    comp_cache_tag_t *tag = NULL;
    uint8_t *read_data = NULL;

    // Search the tag array for a match
    for (int i = 0; i < CACHE_TAGS; i++) {
        if (BDI_CACHE[c_addr->index].tags[i].valid && (BDI_CACHE[c_addr->index].tags[i].tag == c_addr->tag)) {
            tag = &BDI_CACHE[c_addr->index].tags[i];
        }
    }

    // If found, read out the data and pass it to the decompressor
    if (tag != NULL) {
        read_data = malloc(get_comp_size_64(tag->comp_type));
        memcpy(read_data, &BDI_CACHE[c_addr->index].data[(tag->segment)*CACHE_SEGMENT_SIZE], get_comp_size_64(tag->comp_type));

        printf("Read from set %d: compression-type=%04b starting-segment=%d size=%d\n\t", c_addr->index, tag->comp_type, tag->segment, get_comp_size_64(tag->comp_type));
        print_data(read_data, get_comp_size_64(tag->comp_type));
    } else { // If not found, pass the request to main mem then allocate an entry (may need to evict 1+ entries)

    }

    return 0;
}

int write_cache(uint32_t addr, uint8_t *data, comp_type_t comp_type) {
    // Search the tag array for match

    // If found, write new data (may need to evict other entries if size changed)

    //TODO: If not found...
}

int main (int argc, char** argv) {
    for (int i = 0; i < NUM_COMP_TYPE; i++) {
        printf("Compression type %04b base=%d-bits deltas=%d-bits\n", i, get_comp_base(i), get_comp_delta(i));
    }

    uint32_t addr = 0x11223344;
    cache_addr_t *c_addr = (cache_addr_t*)&addr;
    printf("t=%X i=%X o=%X\n", c_addr->tag, c_addr->index, c_addr->offset);
    printf("bits: t=%X i=%X o=%X\n", CACHE_TAG_BITS, CACHE_INDEX_BITS, CACHE_OFFSET_BITS);

    BDI_CACHE[c_addr->index].tags[0].tag = c_addr->tag;
    BDI_CACHE[c_addr->index].tags[0].valid = 1;
    BDI_CACHE[c_addr->index].tags[0].comp_type = COMP_TYPE_B8_D1;
    read_cache(addr);
    return 0;
}