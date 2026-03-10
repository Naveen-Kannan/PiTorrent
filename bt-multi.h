#ifndef BT_MULTI_H
#define BT_MULTI_H

#include "net.h"
#include "dht.h"
#include "boot-crc32.h"

#define CHUNK_SIZE 256
#define FILE_SIZE 4096
#define NUM_CHUNKS (FILE_SIZE / CHUNK_SIZE)
#define CHUNK_FRAG_DATA (MAX_PAYLOAD - 7)

typedef struct {
    uint32_t file_id;
    uint16_t num_chunks;
    uint16_t chunk_size;
    uint32_t chunk_hashes[NUM_CHUNKS];
} __attribute__((packed)) torrent_meta_t;

typedef struct {
    uint32_t node_id;
    uint8_t  chunk_num;
} __attribute__((packed)) chunk_owner_t;

typedef struct {
    uint32_t sender_id;
    uint8_t  chunk_num;
} __attribute__((packed)) bt_request_t;

typedef struct {
    uint32_t sender_id;
    uint8_t  chunk_num;
    uint8_t  offset;
    uint8_t  frag_len;
    uint8_t  data[CHUNK_FRAG_DATA];
} __attribute__((packed)) bt_chunk_t;

typedef struct {
    uint32_t sender_id;
    uint8_t  chunk_num;
} __attribute__((packed)) bt_ack_t;

typedef struct {
    uint32_t file_id;
    uint8_t  have[NUM_CHUNKS];
} __attribute__((packed)) bitfield_payload_t;

typedef struct {
    uint8_t chunk_num;
} __attribute__((packed)) have_payload_t;

typedef struct {
    dht_state_t *dht;
    net_state_t *net;
    uint32_t self_id;

    uint32_t file_id;
    uint32_t chunk_hashes[NUM_CHUNKS];
    uint8_t  data[FILE_SIZE];
    uint8_t  have[NUM_CHUNKS];
    int      is_seed;

    int8_t   chunk_peer[NUM_CHUNKS];
    int      chunks_done;

    uint8_t  peer_have[MAX_CHANNELS][NUM_CHUNKS];
    uint8_t  peer_has_bitfield[MAX_CHANNELS];
    uint8_t  sent_bitfield[MAX_CHANNELS];
} bt_multi_state_t;

void bt_multi_init_from_mac(bt_multi_state_t *bt, net_state_t *net);
void bt_multi_send_bitfield(bt_multi_state_t *bt, int ch_idx);
void bt_multi_broadcast_have(bt_multi_state_t *bt, int chunk_num);
int bt_multi_tick(bt_multi_state_t *bt);

void bt_multi_announce_chunk(bt_multi_state_t *bt, int chunk_num);
void bt_multi_send_chunk(bt_multi_state_t *bt, int peer_ch, int chunk_num);
int  bt_multi_recv_chunk(bt_multi_state_t *bt, int chunk_num, uint32_t timeout_ms);

#endif
