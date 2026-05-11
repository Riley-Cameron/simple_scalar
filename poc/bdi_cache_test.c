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
#define CACHE_SEGMENT_ERR   (CACHE_SEGMENTS+1)
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

/**
 * @brief 32-bit address split into offset, index, and tag
 * 
 */
typedef struct {
    uint32_t offset:CACHE_OFFSET_BITS;
    uint32_t index:CACHE_INDEX_BITS;
    uint32_t tag:CACHE_TAG_BITS;
} cache_addr_t;

/**
 * @brief Contains all info tracked by a single tag entry or 'cache block'
 * 
 */
typedef struct {
    uint64_t tag:CACHE_TAG_BITS;            // This block's tag from the address MSBs
    uint64_t valid:1;                       // Valid bit // TODO: replace with MESI bits?
    uint64_t segment:CACHE_SEGMENT_BITS;    // Segment index where this block begins
    uint64_t zero_bitmask:32;               // Bitmask showing which offsets correspond to the implied zero base (0) and the specified base (1)
    comp_type_t comp_type;                  // Compression type
} comp_cache_tag_t;

/**
 * @brief Structure for a single cache set
 * 
 */
typedef struct {
    comp_cache_tag_t tags[CACHE_TAGS];  // Array of tags contained in the set (calculated as 2 * #ways)
    uint8_t data[CACHE_SET_SIZE];       // Contiguous data storage region for this cache set (combination of all ways)
} comp_cache_set_t;

comp_cache_set_t BDI_CACHE[CACHE_SETS];

/**
 * @brief Get the compression type's base size
 * 
 * @param comp_type 
 * @return uint8_t 
 */
static inline uint8_t get_comp_base(comp_type_t comp_type) {return comp_types[comp_type].base;}

/**
 * @brief Get the compression type's delta size
 * 
 * @param comp_type 
 * @return uint8_t 
 */
static inline uint8_t get_comp_delta(comp_type_t comp_type) {return comp_types[comp_type].delta;}

/**
 * @brief Get the compression type's data size for a 32-byte cache block
 * 
 * @param comp_type 
 * @return uint8_t 
 */
static inline uint8_t get_comp_size_32(comp_type_t comp_type) {return comp_types[comp_type].size_32;}

/**
 * @brief Get the compression type's data size for a 64-byte cache block
 * 
 * @param comp_type 
 * @return uint8_t 
 */
static inline uint8_t get_comp_size_64(comp_type_t comp_type) {return comp_types[comp_type].size_64;}

/**
 * @brief Get the segments required to hold the data for the given compression type (64-byte block mode)
 * 
 * @param comp_type 
 * @return uint8_t 
 */
static inline uint8_t get_segments_req_64(comp_type_t comp_type) {return 1+((get_comp_size_64(comp_type)-1) / CACHE_SEGMENT_SIZE);}

/**
 * @brief Print an array of bytes in hex format
 * 
 * @param data 
 * @param size 
 */
void print_data(uint8_t *data, size_t size) {
    for (int i = 0; i < size; i++) {
        if (i%CACHE_SEGMENT_SIZE==0 && i!=0) {printf("\t");}
        if (i%(CACHE_SEGMENT_SIZE*4)==0 && i!=0) {printf("\n");}
        printf("%02X ", data[i]);
    }
    printf("\n");
}

/**
 * @brief Generate a random & compressed entry for the cache. Parameters are passed by reference. Data is malloced and must be freed by the consumer!!
 * 
 * @param rand_zero_mask 
 * @param rand_comp_type 
 * @return uint8_t* - pointer to the generated data
 */
uint8_t *generate_entry(uint32_t *rand_zero_mask, comp_type_t *rand_comp_type) {
    *rand_zero_mask = (rand() << 15) + rand();
    *rand_comp_type = rand() % NUM_COMP_TYPE;
    uint8_t size = get_comp_size_64(*rand_comp_type);
    uint8_t *rand_data = NULL;

    rand_data = malloc(size);
    if (rand_data == NULL) {
        printf("Error: failed to allocate a data entry\n");
        return NULL;
    }

    for (int i = 0; i < size; i++) {
        rand_data[i] = rand() % 256;
    }

    printf("Generated Random Data: zero-mask=b%32b comp-type=b%04b data=", *rand_zero_mask, *rand_comp_type);
    print_data(rand_data, size);

    return rand_data;
}

/**
 * @brief Searches the given set for a region of contiguous segments that will fit the given compression type.
 * 
 * @param comp_type 
 * @param index 
 * @param tag - to be assigned in this function
 * @return uint16_t - The index of segment at the start of the open section (or CACHE_SEGMENT_ERR if no openings were found)
 */
uint16_t find_available_segments(comp_type_t comp_type, int index, comp_cache_tag_t **tag) {
    *tag = NULL;

    //TODO: make bitmap a cache set parameter?
    uint8_t required_segments = get_segments_req_64(comp_type); // determine how many segments this write will fill
    uint64_t segment_map = 0; // track free vs filled segments in a bitmap
    for (int i = 0; i < CACHE_TAGS; i++) { // search for free segments & a free tag entry
        if (BDI_CACHE[index].tags[i].valid) { // enter each valid entry into the map
            segment_map |= ((1 << (get_segments_req_64(BDI_CACHE[index].tags[i].comp_type)))-1) << BDI_CACHE[index].tags[i].segment;
        } else if (*tag == NULL) { // assign the new entry the first free tag that is found
            *tag = &BDI_CACHE[index].tags[i];
        }
    }

    printf("req-seg: %d seg-map: %032b\n", required_segments, segment_map);

    // If the tag is still null, there are no open tag entries and eviction is required 
    if (*tag == NULL) {
        return CACHE_SEGMENT_ERR;
    }


    // Use the segment map to search for an open region big enough to accomodate the compressed data
    uint8_t avail_segments = 0;
    for (uint16_t i = 0; i < CACHE_SEGMENTS; i++) {
        if (avail_segments == required_segments) {
            return i-required_segments;
        }

        if ((segment_map >> i)&1) {
            avail_segments = 0;
        } else {
            avail_segments++;
        }
    }

    return CACHE_SEGMENT_ERR;
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

        printf("Read from set 0x%03X: compression-type=%04b starting-segment=%d size=%d zero-mask=%0b\n\t", c_addr->index, tag->comp_type, tag->segment, get_comp_size_64(tag->comp_type), tag->zero_bitmask);
        print_data(read_data, get_comp_size_64(tag->comp_type));
        //TODO: pass comp-type, data, and zero-mask to decompression alg
        free(read_data);
    } else { // If not found, pass the request to main mem then allocate an entry (may need to evict 1+ entries)
        //TODO: make "replacement" function
    }

    return 0;
}

int write_cache(uint32_t addr, uint8_t *data, comp_type_t comp_type, uint32_t zero_bitmask) {
    cache_addr_t *c_addr = (cache_addr_t *)&addr;
    comp_cache_tag_t *tag = NULL;

    // Search the tag array for a match
    for (int i = 0; i < CACHE_TAGS; i++) {
        if (BDI_CACHE[c_addr->index].tags[i].valid && (BDI_CACHE[c_addr->index].tags[i].tag == c_addr->tag)) {
            tag = &BDI_CACHE[c_addr->index].tags[i];
        }
    }

    // If found, write new data (may need to evict other entries if size changed)
    if (tag != NULL) {
        // Check if new size is smaller or larger than existing entry
        if (get_comp_size_64(comp_type) <= get_comp_size_64(tag->comp_type)) { // smaller (or equal) size
            memcpy(&BDI_CACHE[c_addr->index].data[(tag->segment)*CACHE_SEGMENT_SIZE], data, get_comp_size_64(comp_type));
            tag->comp_type = comp_type;
            tag->zero_bitmask = zero_bitmask;
        } else { // larger (replacement required!)
            //TODO
            printf("New data is too large to write cleanly into the previous entry! Evicting...\n");

        }
    } else { // If not found 
        uint16_t segment_idx = find_available_segments(comp_type, c_addr->index, &tag);
        if (segment_idx == CACHE_SEGMENT_ERR) { // need to evict to make room
            printf("No segment openings large enough for write data or no tags are available! Evicting...\n");

        } else { // write to the given segment
            memcpy(&BDI_CACHE[c_addr->index].data[segment_idx*CACHE_SEGMENT_SIZE], data, get_comp_size_64(comp_type));
            tag->comp_type = comp_type;
            tag->zero_bitmask = zero_bitmask;
            tag->valid = 1;
            tag->segment = segment_idx;
            tag->tag = ((cache_addr_t*)&addr)->tag;
            printf("seg-idx: %d size: %d tag: 0x%X\n", segment_idx, get_comp_size_64(comp_type), tag->tag);
        }

    }
}

int main (int argc, char** argv) {
    for (int i = 0; i < NUM_COMP_TYPE; i++) {
        printf("Compression type %04b base=%d-bytes deltas=%d-bytes\n", i, get_comp_base(i), get_comp_delta(i));
    }

    uint8_t *data;
    comp_type_t comp_type;
    uint32_t zero_mask;
    uint32_t addr = 0x11223344;
    cache_addr_t *a = (cache_addr_t*)&addr;

    for (int i = 0; i < 16; i++) {
        data = generate_entry(&zero_mask, &comp_type);
        if (data != NULL) {
            write_cache(addr, data, comp_type, zero_mask);
            free(data);
        } else {
            return -1;
        }
        print_data(BDI_CACHE[a->index].data, CACHE_SET_SIZE);
        a->tag++;

        if (i == 8) {a->tag -= 8;}
    }
    return 0;
}