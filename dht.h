#ifndef DHT_H
#define DHT_H

#include "net.h"
#include "kbucket.h"

#define MAX_KV_ENTRIES 64
#define MAX_VALUE_LEN 200

typedef struct {
    uint32_t key;
    uint8_t value[MAX_VALUE_LEN];
    uint16_t value_len;
    int valid;
} kv_entry_t;

typedef struct {
    uint32_t sender_id;
    uint32_t target_id;
} __attribute__((packed)) find_node_req_t;

#define MAX_FIND_RESULTS 4

typedef struct {
    uint32_t node_id;
    uint8_t  channel_hint;
} __attribute__((packed)) node_info_t;

typedef struct {
    uint32_t sender_id;
    uint32_t target_id;
    uint8_t  count;
    node_info_t nodes[MAX_FIND_RESULTS];
} __attribute__((packed)) find_node_resp_t;

typedef struct {
    uint32_t sender_id;
    uint32_t key;
    uint8_t  value_len;
    uint8_t  value[MAX_VALUE_LEN];
} __attribute__((packed)) store_req_t;

typedef struct {
    uint32_t sender_id;
    uint32_t key;
    uint8_t  status;
} __attribute__((packed)) store_ack_t;

typedef struct {
    uint32_t sender_id;
    uint32_t key;
} __attribute__((packed)) find_value_req_t;

typedef struct {
    uint32_t sender_id;
    uint32_t key;
    uint8_t  found;
    uint8_t  value_len;
    uint8_t  value[MAX_VALUE_LEN];
    uint8_t  node_count;
    node_info_t nodes[MAX_FIND_RESULTS];
} __attribute__((packed)) find_value_resp_t;

typedef struct {
    uint32_t self_id;
    net_state_t *net;
    routing_table_t rt;
    kv_entry_t store[MAX_KV_ENTRIES];
    int store_count;
} dht_state_t;

void dht_init(dht_state_t *dht, uint32_t self_id, net_state_t *net);
int dht_put(dht_state_t *dht, uint32_t key, const void *value, int len);
int dht_get(dht_state_t *dht, uint32_t key, void *buf, int *len);
void dht_handle_msg(dht_state_t *dht, int from_peer, packet_t *pkt);
void dht_add_peer(dht_state_t *dht, uint32_t peer_id, int channel_idx);
void dht_bootstrap(dht_state_t *dht);
int dht_local_store(dht_state_t *dht, uint32_t key, const void *value, int len);
int dht_local_get(dht_state_t *dht, uint32_t key, void *buf, int *len);

#endif // DHT_H
