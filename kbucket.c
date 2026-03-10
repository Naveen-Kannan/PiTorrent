#include "kbucket.h"
#include "rpi.h"

static int highest_bit(uint32_t v) {
    if(!v) return -1;
    int pos = 0;
    if(v & 0xFFFF0000) { pos += 16; v >>= 16; }
    if(v & 0xFF00)     { pos += 8;  v >>= 8;  }
    if(v & 0xF0)       { pos += 4;  v >>= 4;  }
    if(v & 0xC)        { pos += 2;  v >>= 2;  }
    if(v & 0x2)        { pos += 1; }
    return pos;
}

int bucket_index(uint32_t self_id, uint32_t node_id) {
    uint32_t dist = self_id ^ node_id;
    if(!dist) return -1;  // same node
    return highest_bit(dist);
}

void rt_init(routing_table_t *rt, uint32_t self_id) {
    rt->self_id = self_id;
    for(int i = 0; i < ID_BITS; i++) {
        rt->buckets[i].count = 0;
        for(int j = 0; j < K_BUCKET_SIZE; j++)
            rt->buckets[i].entries[j].valid = 0;
    }
}

void rt_add(routing_table_t *rt, uint32_t node_id, int channel_idx) {
    int idx = bucket_index(rt->self_id, node_id);
    if(idx < 0) return;

    kbucket_t *b = &rt->buckets[idx];

    for(int i = 0; i < K_BUCKET_SIZE; i++) {
        if(b->entries[i].valid && b->entries[i].node_id == node_id) {
            b->entries[i].channel_idx = channel_idx;
            return;
        }
    }

    if(b->count < K_BUCKET_SIZE) {
        for(int i = 0; i < K_BUCKET_SIZE; i++) {
            if(!b->entries[i].valid) {
                b->entries[i].node_id = node_id;
                b->entries[i].channel_idx = channel_idx;
                b->entries[i].valid = 1;
                b->count++;
                return;
            }
        }
    }
}

int rt_find_closest(routing_table_t *rt, uint32_t target_id,
                    uint32_t *result_ids, int *result_channels, int max_results) {
    uint32_t all_ids[ID_BITS * K_BUCKET_SIZE];
    uint32_t all_dist[ID_BITS * K_BUCKET_SIZE];
    int all_ch[ID_BITS * K_BUCKET_SIZE];
    int total = 0;

    for(int i = 0; i < ID_BITS; i++) {
        kbucket_t *b = &rt->buckets[i];
        for(int j = 0; j < K_BUCKET_SIZE; j++) {
            if(b->entries[j].valid) {
                all_ids[total] = b->entries[j].node_id;
                all_dist[total] = xor_distance(b->entries[j].node_id, target_id);
                all_ch[total] = b->entries[j].channel_idx;
                total++;
            }
        }
    }

    for(int i = 0; i < total - 1; i++) {
        int min_idx = i;
        for(int j = i + 1; j < total; j++) {
            if(all_dist[j] < all_dist[min_idx])
                min_idx = j;
        }
        if(min_idx != i) {
            uint32_t tmp_id = all_ids[i]; all_ids[i] = all_ids[min_idx]; all_ids[min_idx] = tmp_id;
            uint32_t tmp_d = all_dist[i]; all_dist[i] = all_dist[min_idx]; all_dist[min_idx] = tmp_d;
            int tmp_c = all_ch[i]; all_ch[i] = all_ch[min_idx]; all_ch[min_idx] = tmp_c;
        }
    }

    int count = (total < max_results) ? total : max_results;
    for(int i = 0; i < count; i++) {
        result_ids[i] = all_ids[i];
        result_channels[i] = all_ch[i];
    }
    return count;
}

int rt_get_channel(routing_table_t *rt, uint32_t node_id) {
    int idx = bucket_index(rt->self_id, node_id);
    if(idx < 0) return -1;

    kbucket_t *b = &rt->buckets[idx];
    for(int i = 0; i < K_BUCKET_SIZE; i++) {
        if(b->entries[i].valid && b->entries[i].node_id == node_id)
            return b->entries[i].channel_idx;
    }
    return -1;
}

void rt_print(routing_table_t *rt) {
    printk("routing table for node %x:\n", rt->self_id);
    for(int i = 0; i < ID_BITS; i++) {
        kbucket_t *b = &rt->buckets[i];
        if(b->count == 0) continue;
        printk("  bucket[%d]: ", i);
        for(int j = 0; j < K_BUCKET_SIZE; j++) {
            if(b->entries[j].valid)
                printk("(%x ch%d) ", b->entries[j].node_id, b->entries[j].channel_idx);
        }
        printk("\n");
    }
}
