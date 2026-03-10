#ifndef KBUCKET_H
#define KBUCKET_H

#include <stdint.h>

#define ID_BITS 32
#define K_BUCKET_SIZE 2

typedef struct {
    uint32_t node_id;
    int channel_idx;
    int valid;
} kbucket_entry_t;

typedef struct {
    kbucket_entry_t entries[K_BUCKET_SIZE];
    int count;
} kbucket_t;

typedef struct {
    uint32_t self_id;
    kbucket_t buckets[ID_BITS];
} routing_table_t;

static inline uint32_t xor_distance(uint32_t a, uint32_t b) {
    return a ^ b;
}

int bucket_index(uint32_t self_id, uint32_t node_id);
void rt_init(routing_table_t *rt, uint32_t self_id);
void rt_add(routing_table_t *rt, uint32_t node_id, int channel_idx);
int rt_find_closest(routing_table_t *rt, uint32_t target_id,
                    uint32_t *result_ids, int *result_channels, int max_results);
int rt_get_channel(routing_table_t *rt, uint32_t node_id);
void rt_print(routing_table_t *rt);

#endif // KBUCKET_H
