#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

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
#define CACHE_BLOCKS        (CACHE_WAYS*2)
#define CACHE_SET_SIZE      (CACHE_WAYS*CACHE_BLOCK_SIZE)
#define CACHE_SEGMENTS      (CACHE_SET_SIZE/CACHE_SEGMENT_SIZE)
#define CACHE_SEGMENT_ERR   (CACHE_SEGMENTS+1)
#define CACHE_SEGMENT_BITS  (LOG2_8(CACHE_SEGMENTS))

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

char * comp_type_str[NUM_COMP_TYPE] = {
    "ZERO",
    "REPEAT",
    "B8-D1",
    "B8-D2",
    "B8-D4",
    "B4-D1",
    "B4-D1",
    "B2-D1",
    "NONE"
};

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
typedef struct comp_cache_blk {
    uint64_t tag:CACHE_TAG_BITS;            // This block's tag from the address MSBs
    uint64_t valid:1;                       // Valid bit // TODO: replace with MESI bits?
    uint64_t segment:CACHE_SEGMENT_BITS;    // Segment index where this block begins
    uint64_t zero_bitmask:32;               // Bitmask showing which offsets correspond to the implied zero base (0) and the specified base (1)
    comp_type_t comp_type;                  // Compression type
    struct comp_cache_blk *way_next;	    // Next block in the ordered way chain (towards the tail), used to order blocks for replacement */
    struct comp_cache_blk *way_prev;	    // Previous block in the order way chain (towards the head) */
} comp_cache_blk_t;

/**
 * @brief Structure for a single cache set
 * 
 */
typedef struct {
    comp_cache_blk_t blks[CACHE_BLOCKS];  // Array of blocks contained in the set (calculated as 2 * #ways)
    comp_cache_blk_t *way_head;
    comp_cache_blk_t *way_tail;
    uint8_t data[CACHE_SET_SIZE];       // Contiguous data storage region for this cache set (combination of all ways)
} comp_cache_set_t;

comp_cache_set_t BDI_CACHE[CACHE_SETS];

typedef struct {
    int64_t base;
    int32_t deltas[32];
    int32_t zero_bitmask;
    comp_type_t comp_type;
} comp_data_t; 

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
 * @brief Print the LRU order of the given set
 * 
 * @param set 
 */
void print_lru_order(comp_cache_set_t *set) {
    comp_cache_blk_t *blk = set->way_head;
    printf("Head (MRU) -> ");
    while (blk != NULL) {
        printf("0x%X ", blk->tag);
        blk = blk->way_next;
    }
    printf("<- Tail (LRU)\n");
}

typedef enum { Head, Tail } list_loc_t;
/**
 * @brief insert BLK into the order way chain in SET at location WHERE
 * 
 * @param set 
 * @param blk 
 * @param where 
 */
static void update_way_list(comp_cache_set_t *set, comp_cache_blk_t *blk, list_loc_t where) {
    /* unlink entry from the way list */
    if (!blk->way_prev && !blk->way_next) {
        /* only one entry in list (direct-mapped), no action */
        /* Head/Tail order already */
        return;
    } else if (!blk->way_prev) { /* else, more than one element in the list */
        if (where == Head) {
            /* already there */
            return;
        }
        /* else, move to tail */
        set->way_head = blk->way_next;
        blk->way_next->way_prev = NULL;
    } else if (!blk->way_next) {
        /* end of list (and not front of list) */
        if (where == Tail) {
            /* already there */
            return;
        }
        set->way_tail = blk->way_prev;
        blk->way_prev->way_next = NULL;
    } else {
      /* middle of list (and not front or end of list) */
      blk->way_prev->way_next = blk->way_next;
      blk->way_next->way_prev = blk->way_prev;
    }

    /* link BLK back into the list */
    if (where == Head) {
        /* link to the head of the way list */
        blk->way_next = set->way_head;
        blk->way_prev = NULL;
        set->way_head->way_prev = blk;
        set->way_head = blk;
    } else if (where == Tail) {
        /* link to the tail of the way list */
        blk->way_prev = set->way_tail;
        blk->way_next = NULL;
        set->way_tail->way_next = blk;
        set->way_tail = blk;
    } else {
        printf("bogus WHERE designator\n");
    }
}

static int64_t get_rand64() {
    uint64_t r = 0;
    // rand() typically returns up to 0x7FFF. 
    // We shift and bitwise-OR to safely build a 64-bit integer.
    for (int i = 0; i < 5; i++) {
        r = (r << 15) | (rand() & 0x7FFF);
    }
    return (int64_t)r;
}

/**
 * @brief Generate a random & compressed entry for the cache. Parameters are passed by reference. Data is malloced and must be freed by the consumer!!
 * 
 * @param rand_zero_mask 
 * @param rand_comp_type 
 * @return uint8_t* - pointer to the generated data
 */
uint8_t *generate_entry() {
    uint32_t rand_zero_mask = (rand() << 15) + rand();
    comp_type_t rand_comp_type = rand() % NUM_COMP_TYPE;
    uint8_t size = 64;
    uint8_t *rand_data = NULL;

    rand_data = malloc(size);
    if (rand_data == NULL) {
        printf("Error: failed to allocate a data entry\n");
        return NULL;
    }

    switch (rand_comp_type) {
    case COMP_TYPE_ZERO:
        memset(rand_data, 0, size);
        break;
    case COMP_TYPE_REP_VAL:
        uint8_t rand_byte = rand();
        memset(rand_data, rand_byte, size);
        break;
    case COMP_TYPE_NONE:
        for (int i = 0; i < size; i++) {
            rand_data[i] = rand();
        }
        break;
    default:
        int64_t base = get_rand64();
        uint8_t base_size = get_comp_base(rand_comp_type);
        uint8_t delta_size = get_comp_delta(rand_comp_type);

        printf("base=0x%lX\n", base);
        for (int i = 0; i < base_size; i++) {
            rand_data[i] = (base >> (base_size-i-1)*8) & 0xFF;
        }

        for (int i = base_size; i < size; i+=base_size) {
            int64_t delta;
            if (delta_size >= 8) {
                delta = get_rand64(); // Prevent shift overflow if delta_size is 8
            } else {
                uint64_t raw_rand = (uint64_t)get_rand64();
                uint64_t mask = (1ULL << (delta_size * 8)) - 1;       // e.g., 0xFF for 1-byte
                uint64_t half_val = 1ULL << ((delta_size * 8) - 1);   // e.g., 0x80 (128) for 1-byte
                
                // Mask to get positive bounds, then shift down by half to allow negative deltas
                delta = (int64_t)((raw_rand & mask) - half_val); 
            }

            bool zero_base = !(rand_zero_mask & (1ULL << i/base_size));

            int64_t entry = zero_base ? delta : base+delta;
            printf("delta=0x%016lX (%ld)\tentry=0x%016lX\n", delta, delta, entry);
            for (int j = 0; j < base_size; j++) {
                rand_data[i+j] = (entry >> (base_size-j-1)*8) & 0xFF;
            }
        }
        break;
    }

    printf("Generated Random Data (comp-type=%s): ", comp_type_str[rand_comp_type]);
    print_data(rand_data, size);

    return rand_data;
}

/**
 * @brief Searches the given set for a region of contiguous segments that will fit the given compression type.
 * 
 * @param comp_type 
 * @param index 
 * @param blk - to be assigned in this function
 * @return uint16_t - The index of segment at the start of the open section (or CACHE_SEGMENT_ERR if no openings were found)
 */
uint16_t find_available_segments(comp_type_t comp_type, int index, comp_cache_blk_t **blk) {
    // assign the block if it is NULL
    if (*blk == NULL && !BDI_CACHE[index].way_tail->valid) {
        *blk = BDI_CACHE[index].way_tail;
    } else { // If the block is still null, there are no open tag entries and eviction is required 
        return CACHE_SEGMENT_ERR;
    }

    //TODO: make bitmap a cache set parameter?
    uint8_t required_segments = get_segments_req_64(comp_type); // determine how many segments this write will fill
    uint64_t segment_map = 0; // track free vs filled segments in a bitmap
    for (int i = 0; i < CACHE_BLOCKS; i++) { // search for free segments & a free tag entry
        if (BDI_CACHE[index].blks[i].valid) { // enter each valid entry into the map
            segment_map |= ((1 << (get_segments_req_64(BDI_CACHE[index].blks[i].comp_type)))-1) << BDI_CACHE[index].blks[i].segment;
        }
    }

    printf("req-seg: %d seg-map: %032lb\n", required_segments, segment_map);

    // Use the segment map to search for an open region big enough to accomodate the compressed data
    uint8_t avail_segments = 0;
    for (uint16_t i = 0; i < CACHE_SEGMENTS; i++) {
        if ((segment_map >> i)&1) {
            avail_segments = 0;
        } else {
            avail_segments++;
        }
        if (avail_segments == required_segments) {
            return i+1-required_segments;
        }
    }

    return CACHE_SEGMENT_ERR;
}

/**
 * @brief 
 * 
 * @param comp_type - compression type of the block to be inserted
 * @param index - index of the set
 * @param blk - will be assigned if NULL
 * @return uint16_t - segment index of the location to insert the new block
 */
uint16_t evict_cache(comp_type_t comp_type, int index, comp_cache_blk_t **blk) {
    if (*blk == NULL) { // If the block was not already assigned, it means there are no available ones
        *blk = BDI_CACHE[index].way_tail; // evict the LRU block
        (*blk)->valid = 0; // invalidate this block for mapping
        printf("evicted block: tag=0x%X\n", (*blk)->tag);
    }

    // Build a segment bitmap
    uint8_t required_segments = get_segments_req_64(comp_type); // determine how many segments this write will fill
    uint64_t segment_map = 0; // track free vs filled segments in a bitmap
    for (int i = 0; i < CACHE_BLOCKS; i++) { // search for free segments & a free tag entry
        if (BDI_CACHE[index].blks[i].valid) { // enter each valid entry into the map
            segment_map |= ((1 << (get_segments_req_64(BDI_CACHE[index].blks[i].comp_type)))-1) << BDI_CACHE[index].blks[i].segment;
        }
    }

    printf("req-seg: %d seg-map: %032lb\n", required_segments, segment_map);

    // Use the segment map to search for an open region big enough to accomodate the compressed data
    // TODO: prioritize LRU when Segments are full!
    uint8_t avail_segments = 0;
    uint8_t max_avail_segments = 0;
    uint16_t max_avail_segments_idx = 0;
    for (uint16_t i = 0; i < CACHE_SEGMENTS; i++) {
        if ((segment_map >> i)&1) {
            avail_segments = 0;
        } else {
            avail_segments++;
        }
        if (avail_segments >= required_segments) {
            return i+1-required_segments; // If we evicted a tag, it may have freed up enough space to fit the data
        }
        // Track the maximum available segments, but make sure we don't get too close to the end of the segment area!!
        if (avail_segments > max_avail_segments && (i+1-avail_segments+required_segments)<CACHE_SEGMENTS) { 
            max_avail_segments = avail_segments;
            max_avail_segments_idx = i+1-avail_segments;
        }
    }

    // Clear out any subsequent blocks need to make room for the new one
    while (max_avail_segments < required_segments) {
        // Find next block
        for (int i = 0; i < CACHE_BLOCKS; i++) {
            if ((BDI_CACHE[index].blks[i].valid) && (BDI_CACHE[index].blks[i].segment == (max_avail_segments_idx+max_avail_segments))) {
                // Evict it
                update_way_list(&BDI_CACHE[index], &BDI_CACHE[index].blks[i], Tail);

                // Update the available segments count
                max_avail_segments += get_segments_req_64(BDI_CACHE[index].blks[i].comp_type);
                BDI_CACHE[index].blks[i].tag = 0;
                BDI_CACHE[index].blks[i].valid = 0;

                // check segment map for free segments before the next filled ones
                while ((max_avail_segments < required_segments) && !((segment_map >> (max_avail_segments_idx+max_avail_segments))&1)) {
                    max_avail_segments++;
                    printf("found empty segment after eviction\n");
                }

                printf("evicted block: tag=0x%X start-seg=%d avail-seg=%d\n", BDI_CACHE[index].blks[i].tag, max_avail_segments_idx, max_avail_segments);
                break;
            }
        }
    }

    return max_avail_segments_idx;
}

//base 8 delta 1 check
comp_data_t check_B8D1(uint8_t *data){

    comp_data_t comp_data = {0};
    comp_data.comp_type = COMP_TYPE_NONE;
    uint64_t values[8] = {0};
    uint32_t zero_bitmask = 0;
    int64_t arb_base = 0;
    int bdi_count = 0;
    bool arb_base_set = false;
    for(int i=0; i<64; i=i+8){
        uint64_t val = 0;
        for(int j=i; j<i+8; j++){
            val = val << 8 | data[j];
        }
        values[i/8] = val;
        int64_t delta = (int64_t)val;
        if(delta >= -128 && delta <= 127){
            zero_bitmask = zero_bitmask | (0x80U >> i/8);
            comp_data.deltas[i/8] = (int32_t) delta;
        }
        else {
            if(!arb_base_set) {
                arb_base = (int64_t) val;
                arb_base_set = true;
            }
        }
    }

    for(int i=0;i<8;i++){
        if(zero_bitmask & (0x80U >> i)){
            bdi_count++;
        }
        else{
            int64_t sdiff = (int64_t)values[i] - arb_base;
            if (sdiff >= -128 && sdiff <= 127) {
                bdi_count++;
                comp_data.deltas[i] = (int32_t)sdiff;
            }
        }
    }

    if (bdi_count == 8) {
        comp_data.base = (int64_t)arb_base;
        comp_data.comp_type = COMP_TYPE_B8_D1;
        comp_data.zero_bitmask = zero_bitmask;
    }

    return comp_data;
}
    
//check base 4 delta 1
comp_data_t check_B4D1(uint8_t *data){

    comp_data_t comp_data ={0};
    comp_data.comp_type = COMP_TYPE_NONE;
    bool arb_base_set = false;
    uint16_t zero_bitmask = 0;
    int32_t arb_base = 0;
    int bdi_count = 0;
    uint32_t values[16] = {0};
    for(int i=0; i<64; i=i+4){
        uint32_t val = 0;
        for(int j=i; j<i+4; j++){
            val = (val << 8) | data[j];
        }
        values[i/4] = val;
        int32_t sval = (int32_t) val;
        int64_t delta = (int64_t) sval;
        if( delta >= -128 && delta <= 127){
            zero_bitmask = zero_bitmask | (0x8000U >> i/4);
            comp_data.deltas[i/4] = (int32_t) delta;
        }
        else {
            if(!arb_base_set){
                arb_base = (int32_t)val;
                arb_base_set = true;
            }
        }
    }

    for(int i=0; i<16; i++){
        if(zero_bitmask & (0x8000U >> i)){
            bdi_count++;
        }
        else {
            int32_t sdiff = (int32_t)values[i] - arb_base;
            if(sdiff >= -128 && sdiff <= 127){
                bdi_count++;
                comp_data.deltas[i] = (int32_t)sdiff;
            }
        }
    }

    if(bdi_count == 16){
        comp_data.base = arb_base;
        comp_data.comp_type = COMP_TYPE_B4_D1;
        comp_data.zero_bitmask = zero_bitmask;
    }

    return comp_data;
}

//check base 8 delta 2
comp_data_t check_B8D2(uint8_t *data) {
    
    comp_data_t comp_data ={0};
    comp_data.comp_type = COMP_TYPE_NONE;
    bool arb_base_set = false;
    uint32_t zero_bitmask = 0;
    int64_t arb_base = 0;
    int bdi_count = 0;
    uint64_t values[8] = {0};
    for(int i=0; i<64; i=i+8){
        uint64_t val = 0;
        for(int j=i; j<i+8; j++){
            val = (val << 8) | data[j];
        }
        values[i/8] = val;
        int64_t delta = (int64_t) val;
        if( delta >= -32768 && delta <= 32767){
            zero_bitmask = zero_bitmask | (0x80U >> i/8);
            comp_data.deltas[i/8] = (int32_t) delta;
        }
        else {
            if(!arb_base_set){
                arb_base = (int64_t) val;
                arb_base_set = true;
            }
        }
    }

    for(int i=0; i<8; i++){
        if(zero_bitmask & (0x80U >> i)){
            bdi_count++;
        }
        else {
            int64_t sdiff = (int64_t)values[i] - arb_base;
            if(sdiff >= -32768 && sdiff <= 32767){
                bdi_count++;
                comp_data.deltas[i] = (int32_t)sdiff;
            }
        }
    }

    if(bdi_count == 8){
        comp_data.base = arb_base;
        comp_data.comp_type = COMP_TYPE_B8_D2;
        comp_data.zero_bitmask = zero_bitmask;
    }

    return comp_data;
}

//check base 2 delta 1
comp_data_t check_B2D1(uint8_t *data){

    comp_data_t comp_data = {0};
    comp_data.comp_type = COMP_TYPE_NONE;
    bool arb_base_set = false;
    uint32_t zero_bitmask = 0;
    int16_t arb_base = 0;
    int bdi_count = 0;
    uint16_t values[32] = {0};

    for(int i=0; i<64; i=i+2){
        uint16_t val = 0;
        for(int j=i; j<i+2; j++){
            val = (val << 8) | data[j];
        }
        values[i/2] = val;
        int16_t sval = (int16_t) val;
        int32_t delta  = (int32_t) sval;
        if(delta >= -128 && delta <= 127){
            zero_bitmask = zero_bitmask | (0x80000000U >> i/2);
            comp_data.deltas[i/2] = (int32_t) delta;
        }
        else{
            if(!arb_base_set){
                arb_base = (int16_t) val;
                arb_base_set = true;
            }
        }
    }

    for(int i=0; i<32; i++){
        if(zero_bitmask & (0x80000000U >> i)){
            bdi_count++;
        }
        else{
            int16_t sdiff = (int16_t)values[i] - arb_base;
            if(sdiff >= -128 && sdiff <= 127){
                bdi_count++;
                comp_data.deltas[i] = (int32_t)sdiff;
            }
        }
    }

    if(bdi_count == 32){
        comp_data.base = arb_base;
        comp_data.comp_type = COMP_TYPE_B2_D1;
        comp_data.zero_bitmask = zero_bitmask;
    }

    return comp_data;
}

//check base 4 delta 2
comp_data_t check_B4D2(uint8_t *data){

    comp_data_t comp_data = {0};
    comp_data.comp_type = COMP_TYPE_NONE;
    bool arb_base_set = false;
    uint16_t zero_bitmask = 0;
    int32_t arb_base = 0;
    int bdi_count = 0;
    uint32_t values[16] = {0};
    for(int i=0; i<64; i=i+4){
        uint32_t val = 0;
        for(int j=i; j<i+4; j++){
            val = (val << 8) | data[j];
        }
        values[i/4] = val;
        int32_t delta = (int32_t) val;
        if( delta >= -32768 && delta <= 32767){
            zero_bitmask = zero_bitmask | (0x8000U >> i/4);
            comp_data.deltas[i/4] = (int32_t) delta;
        }
        else {
            if(!arb_base_set){
                arb_base = (int32_t)val;
                arb_base_set = true;
            }
        }
    }

    for(int i=0; i<16; i++){
        if(zero_bitmask & (0x8000U >> i)){
            bdi_count++;
        }
        else {
            int32_t sdiff = (int32_t)values[i] - arb_base;
            if(sdiff >= -32768 && sdiff <= 32767){
                bdi_count++;
                comp_data.deltas[i] = (int32_t)sdiff;
            }
        }
    }

    if(bdi_count == 16){
        comp_data.base = arb_base;
        comp_data.comp_type = COMP_TYPE_B4_D2;
        comp_data.zero_bitmask = zero_bitmask;
    }

    return comp_data;
}

//check base 8 delta 4
comp_data_t check_B8D4(uint8_t *data){
    comp_data_t comp_data = {0};
    comp_data.comp_type = COMP_TYPE_NONE;
    bool arb_base_set = false;
    uint32_t zero_bitmask = 0;
    int64_t arb_base = 0;
    int bdi_count = 0;
    uint64_t values[8] = {0};
    for(int i=0; i<64; i=i+8){
        uint64_t val = 0;
        for(int j=i; j<i+8; j++){
            val = (val << 8) | data[j];
        }
        values[i/8] = val;
        int64_t delta = (int64_t) val;
        if( delta >= INT32_MIN && delta <= INT32_MAX){
            zero_bitmask = zero_bitmask | (0x80U >> i/8);
            comp_data.deltas[i/8] = (int32_t) delta;
        }
        else {
            if(!arb_base_set){
                arb_base = (int64_t) val;
                arb_base_set = true;
            }
        }
    }

    for(int i=0; i<8; i++){
        if(zero_bitmask & (0x80U >> i)){
            bdi_count++;
        }
        else {
            int64_t sdiff = (int64_t) values[i] - arb_base;
            if(sdiff >= INT32_MIN && sdiff <= INT32_MAX){
                bdi_count++;
                comp_data.deltas[i] = (int32_t)sdiff;
            }
        }
    }

    if(bdi_count == 8){
        comp_data.base = arb_base;
        comp_data.comp_type = COMP_TYPE_B8_D4;
        comp_data.zero_bitmask = zero_bitmask;
    }

    return comp_data;
}

comp_data_t compress_data(uint8_t *data) {

    uint32_t zero_bitmask = 0;
    uint64_t arb_base8 = 0;
    uint32_t arb_base4 = 0;
    bool arb_base_set = false;
    int bdi_count = 0;
    int sum = 0;
    bool rep = true;
    comp_data_t comp_data = {0};
    comp_data.comp_type = COMP_TYPE_NONE;

    for(int i=0; i<64; i++){
        sum += data[i];
    }

    if(sum == 0) {
        comp_data.comp_type = COMP_TYPE_ZERO;
        return comp_data;
    }

    uint64_t rep_val[8];
    for(int i=0; i<64; i=i+8){
        uint64_t val = 0;
        for(int j=i; j<i+8; j++){
            val = (val << 8) | data[j]; 
        }
        rep_val[i/8] = val;
    }

    for(int i=0; i<8; i++){
        if(rep_val[0] != rep_val[i]){
            rep = false;
            break;
        }
    }

    if(rep) {
        comp_data.base = rep_val[0];
        comp_data.comp_type = COMP_TYPE_REP_VAL;
        return comp_data;
    }

    comp_data = check_B8D1(data);
    if(comp_data.comp_type == COMP_TYPE_B8_D1) return comp_data;

    comp_data = check_B4D1(data);
    if(comp_data.comp_type == COMP_TYPE_B4_D1) return comp_data;

    comp_data = check_B8D2(data);
    if(comp_data.comp_type == COMP_TYPE_B8_D2) return comp_data;

    comp_data = check_B2D1(data);
    if(comp_data.comp_type == COMP_TYPE_B2_D1) return comp_data;

    comp_data = check_B4D2(data);
    if(comp_data.comp_type == COMP_TYPE_B4_D2) return comp_data;

    comp_data = check_B8D4(data);
    if(comp_data.comp_type == COMP_TYPE_B8_D4) return comp_data;

    return comp_data;
   
}

/**
 * @brief Forward declaration of bdi_decompress (defined in bdi_decompression.c)
 *
 * @param comp_type    Compression encoding (from the tag entry)
 * @param zero_bitmask Per-element base selector bitmask (from the tag entry)
 * @param cb           Pointer to compressed bytes in segment storage (big-endian)
 * @param out          Output buffer (must be CACHE_BLOCK_SIZE bytes)
 * @return int         0 on success, -1 on unsupported comp_type
 */
int bdi_decompress(comp_type_t comp_type, int32_t zero_bitmask,
                   const uint8_t *cb, uint8_t out[CACHE_BLOCK_SIZE]);

/**
 * @brief Pack a comp_data_t into segment storage as a big-endian compressed byte stream.
 *        Layout: [base: base_size bytes, BE] [delta_0: delta_size bytes, BE] ...
 *        COMP_TYPE_ZERO and COMP_TYPE_NONE are handled by the caller via memcpy.
 *
 * @param cd  Source comp_data_t produced by compress_data()
 * @param dst Destination buffer in segment storage
 */
static void pack_comp_data(const comp_data_t *cd, uint8_t *dst)
{
    uint8_t base_size  = get_comp_base(cd->comp_type);
    uint8_t delta_size = get_comp_delta(cd->comp_type);
    uint8_t n_elems    = CACHE_BLOCK_SIZE / base_size;

    /* write base big-endian */
    for (int b = 0; b < base_size; b++)
        dst[b] = (cd->base >> (base_size - b - 1) * 8) & 0xFF;

    /* write each delta big-endian at delta_size width */
    for (int i = 0; i < n_elems; i++)
        for (int b = 0; b < delta_size; b++)
            dst[base_size + i * delta_size + b] =
                (cd->deltas[i] >> (delta_size - b - 1) * 8) & 0xFF;
}

/**
 * @brief Write into the L2 compressed cache
 * 
 * @param addr 
 * @param data 
 * @return int - success
 */
int write_cache(uint32_t addr, uint8_t *data) {
    cache_addr_t *c_addr = (cache_addr_t *)&addr;
    comp_cache_blk_t *blk = NULL;

    comp_data_t compressed_data = {0};

    compressed_data =  compress_data(data);
    comp_type_t comp_type = compressed_data.comp_type;
    uint32_t zero_bitmask = compressed_data.zero_bitmask;
    printf("zero-mask=b%32b comp-type=%s\n", zero_bitmask, comp_type_str[comp_type]);
    // Search the tag array for a match
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        if (BDI_CACHE[c_addr->index].blks[i].valid && (BDI_CACHE[c_addr->index].blks[i].tag == c_addr->tag)) {
            blk = &BDI_CACHE[c_addr->index].blks[i];
        }
    }

    // If found, write new data (may need to evict other entries if size changed)
    if (blk != NULL) {
        // Check if new size is smaller or larger than existing entry
        if (get_comp_size_64(comp_type) <= get_comp_size_64(blk->comp_type)) { // smaller (or equal) size
            if (comp_type == COMP_TYPE_NONE) {
                memcpy(&BDI_CACHE[c_addr->index].data[blk->segment * CACHE_SEGMENT_SIZE],
                       data, get_comp_size_64(comp_type));
            } else {
                pack_comp_data(&compressed_data,
                               &BDI_CACHE[c_addr->index].data[blk->segment * CACHE_SEGMENT_SIZE]);
            }
            blk->comp_type = comp_type;
            blk->zero_bitmask = zero_bitmask;
            printf("\033[32m[WRITE HIT] (tag=0x%X) seg-idx: %d size: %d\n\033[0m", blk->tag, blk->segment, get_comp_size_64(comp_type));
        } else { // larger (replacement required!)
            printf("\033[33m[WRITE HIT] (tag=0x%X) New data is too large to write cleanly into the previous entry! Evicting...\n\033[0m", c_addr->tag);
            uint16_t segment_idx = evict_cache(comp_type, c_addr->index, &blk);

            // write to the given segment
            if (comp_type == COMP_TYPE_NONE) {
                memcpy(&BDI_CACHE[c_addr->index].data[segment_idx * CACHE_SEGMENT_SIZE],
                       data, get_comp_size_64(comp_type));
            } else {
                pack_comp_data(&compressed_data,
                               &BDI_CACHE[c_addr->index].data[segment_idx * CACHE_SEGMENT_SIZE]);
            }
            blk->comp_type = comp_type;
            blk->zero_bitmask = zero_bitmask;
            blk->valid = 1;
            blk->segment = segment_idx;
            blk->tag = ((cache_addr_t*)&addr)->tag; 
            printf("seg-idx: %d size: %d tag: 0x%X\n", segment_idx, get_comp_size_64(comp_type), blk->tag);
        }
    } else { // If not found 
        uint16_t segment_idx = find_available_segments(comp_type, c_addr->index, &blk);
        if (segment_idx == CACHE_SEGMENT_ERR) { // need to evict to make room
            printf("No segment openings large enough for write data OR no tags are available! Evicting...\n");
            segment_idx = evict_cache(comp_type, c_addr->index, &blk);
        }
        
        // write to the given segment
        if (comp_type == COMP_TYPE_NONE) {
            memcpy(&BDI_CACHE[c_addr->index].data[segment_idx * CACHE_SEGMENT_SIZE],
                   data, get_comp_size_64(comp_type));
        } else {
            pack_comp_data(&compressed_data,
                           &BDI_CACHE[c_addr->index].data[segment_idx * CACHE_SEGMENT_SIZE]);
        }
        blk->comp_type = comp_type;
        blk->zero_bitmask = zero_bitmask;
        blk->valid = 1;
        blk->segment = segment_idx;
        blk->tag = ((cache_addr_t*)&addr)->tag; 
        printf("\033[31m[WRITE MISS] (tag=0x%X) seg-idx: %d size: %d\n\033[0m",  blk->tag, segment_idx, get_comp_size_64(comp_type));
    }

    // Update LRU order
    update_way_list(&(BDI_CACHE[c_addr->index]), blk, Head); 
    return 0;
}

/**
 * @brief Read from the L2 compressed cache
 * 
 * @param addr 
 * @return int - success
 */
int read_cache(uint32_t addr) {
    cache_addr_t *c_addr = (cache_addr_t *)&addr;
    comp_cache_blk_t *blk = NULL;

    // Search the tag array for a match
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        if (BDI_CACHE[c_addr->index].blks[i].valid && (BDI_CACHE[c_addr->index].blks[i].tag == c_addr->tag)) {
            blk = &BDI_CACHE[c_addr->index].blks[i];
        }
    }

    // If found, read out the data and pass it to the decompressor
    if (blk != NULL) {
        uint8_t decompressed[CACHE_BLOCK_SIZE];
        int ret = bdi_decompress(blk->comp_type,
                                 (int32_t)blk->zero_bitmask,
                                 &BDI_CACHE[c_addr->index].data[blk->segment * CACHE_SEGMENT_SIZE],
                                 decompressed);
        update_way_list(&BDI_CACHE[c_addr->index], blk, Head); // Update LRU order
        printf("\033[32m[READ HIT] (tag=0x%X) compression-type=%s starting-segment=%d size=%d zero-mask=%08X\n\033[0m",
               c_addr->tag, comp_type_str[blk->comp_type], blk->segment,
               get_comp_size_64(blk->comp_type), (uint32_t)blk->zero_bitmask);
        if (ret == 0)
            print_data(decompressed, CACHE_BLOCK_SIZE);
        else
            printf("[DECOMP ERROR] ret=%d\n", ret);
    } else { // If not found, pass the request to main mem then allocate an entry (may need to evict 1+ entries)
        printf("\033[31m[READ MISS] (tag=0x%X) Writing data from main mem into L2\n\033[0m", c_addr->tag);

        // Simulate getting data from main mem
        uint8_t *read_data = generate_entry();

        // Write it into the cache
        write_cache(addr, read_data);
        free(read_data);
    }

    return 0;
}

int main (int argc, char** argv) {
    // Initialize all cache block linked lists (arbitrary order)
    for (int i = 0; i < CACHE_SETS; i++) {
        BDI_CACHE[i].way_head = &BDI_CACHE[i].blks[0];
        BDI_CACHE[i].way_tail = &BDI_CACHE[i].blks[CACHE_BLOCKS-1];

        BDI_CACHE[i].blks[0].way_prev = NULL; // There is nothing before the head
        if (CACHE_BLOCKS > 1) {
            BDI_CACHE[i].blks[0].way_next = &BDI_CACHE[i].blks[1];
        }
        BDI_CACHE[i].blks[CACHE_BLOCKS-1].way_next = NULL; // There is nothing before the tail
        if (CACHE_BLOCKS > 1) {
            BDI_CACHE[i].blks[CACHE_BLOCKS-1].way_prev = &BDI_CACHE[i].blks[CACHE_BLOCKS-2];
        }
        
        if (CACHE_BLOCKS > 2) { // Init middle entries
            for (int j = 1; j < (CACHE_BLOCKS-1); j++) {
                BDI_CACHE[i].blks[j].way_prev = &BDI_CACHE[i].blks[j-1];
                BDI_CACHE[i].blks[j].way_next = &BDI_CACHE[i].blks[j+1];
            }
        }
    }

    // Print out compression type info
    for (int i = 0; i < NUM_COMP_TYPE; i++) {
        printf("Compression type %s: base=%d-bytes deltas=%d-bytes\n", comp_type_str[i], get_comp_base(i), get_comp_delta(i));
    }

    uint8_t *data;
    uint32_t addr = 0x11223344;
    cache_addr_t *a = (cache_addr_t*)&addr;

    // Test Writes
    for (int i = 0; i < 16; i++) {
        printf("\n-----\n\n");
        data = generate_entry();
        if (data != NULL) {
            write_cache(addr, data);
            free(data);
        } else {
            return -1;
        }
        print_lru_order(&BDI_CACHE[a->index]);
        print_data(BDI_CACHE[a->index].data, CACHE_SET_SIZE);
        a->tag++;

        if (i == 7) {a->tag -= 8;}     
    }

    a->tag = 0x1122;

    // Test Reads - first 8 should be hits, then we should start missing and overwriting the set with new fills
    for (int i = 0; i < 16; i++) {
        printf("\n-----\n\n");
        read_cache(addr);
        print_lru_order(&BDI_CACHE[a->index]);
        print_data(BDI_CACHE[a->index].data, CACHE_SET_SIZE);
        a->tag++;
    }

    return 0;
}
