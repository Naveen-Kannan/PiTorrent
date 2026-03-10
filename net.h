#ifndef NET_H
#define NET_H

#include "rpi.h"
#include "boot-crc32.h"
#include "bt.h"

#define MAX_CHANNELS    7
#define MAX_DISCOVERED  8
#define PKT_QUEUE_SIZE  8

#define MAX_PEERS       4
#define MAX_KNOWN_PEERS 16

#define INQUIRY_INTERVAL_MS  30000
#define SHORT_INQUIRY_LEN    2

#define PREAMBLE    0xAA
#define MAX_PAYLOAD 200

#define MSG_PING       0x01
#define MSG_PONG       0x02
#define MSG_HELLO      0x03
#define MSG_HELLO_ACK  0x04
#define MSG_REQUEST    0x05
#define MSG_CHUNK      0x06
#define MSG_ACK        0x07
#define MSG_DONE       0x08

#define MSG_ANNOUNCE     0x10
#define MSG_ANNOUNCE_ACK 0x11

#define MSG_PEER_LIST      0x12
#define MSG_PEER_LIST_RESP 0x13

#define MSG_FIND_NODE       0x20
#define MSG_FIND_NODE_RESP  0x21
#define MSG_STORE           0x22
#define MSG_STORE_ACK       0x23
#define MSG_FIND_VALUE      0x24
#define MSG_FIND_VALUE_RESP 0x25

#define MSG_BITFIELD  0x30
#define MSG_HAVE      0x31

#define MSG_FORWARD   0x40

#define NET_OK           0
#define NET_TIMEOUT     -1
#define NET_BAD_CHECKSUM -2
#define NET_NOT_READY   -3

typedef struct {
    uint8_t preamble;
    uint8_t len;
    uint8_t type;
    uint8_t payload[MAX_PAYLOAD];
    uint8_t checksum;
} __attribute__((packed)) packet_t;

#define FORWARD_HDR_SIZE 11

typedef struct {
    uint32_t dest_id;
    uint32_t src_id;
    uint8_t  ttl;
    uint8_t  inner_type;
    uint8_t  inner_len;
    uint8_t  inner_payload[MAX_PAYLOAD - FORWARD_HDR_SIZE];
} __attribute__((packed)) forward_t;

typedef struct {
    uint16_t acl_handle;
    uint8_t  bd_addr[6];
    int      connected;

    packet_t queue[PKT_QUEUE_SIZE];
    volatile int q_head, q_tail;
} channel_t;

typedef struct {
    uint8_t  bd_addr[6];
    uint32_t node_id;
    int      channel_idx;
    int      next_hop_ch;
    int      valid;
} known_peer_t;

typedef struct {
    uint8_t  bd_addr[6];
    uint32_t node_id;
} __attribute__((packed)) peer_entry_t;

typedef struct {
    uint8_t count;
    peer_entry_t peers[MAX_KNOWN_PEERS];
} __attribute__((packed)) peer_list_payload_t;

typedef struct {
    channel_t ch[MAX_CHANNELS];
    int ch_active[MAX_CHANNELS];
    int num_active;
    uint32_t node_id;

    uint8_t self_addr[6];

    known_peer_t known[MAX_KNOWN_PEERS];
    int num_known;

    int      discovery_active;
    uint32_t last_inquiry_us;
    uint32_t last_gossip_us;
    int      gossip_cursor;

    int new_connection;
} net_state_t;

void net_init(net_state_t *n);
void net_int_init(net_state_t *n);
void net_start_discovery(net_state_t *n);
int  net_discovery_tick(net_state_t *n);
void net_gossip_tick(net_state_t *n);
int  net_send(net_state_t *n, int ch_idx, packet_t *pkt, uint32_t timeout_ms);
int  net_recv(net_state_t *n, int *from_ch, packet_t *pkt, uint32_t timeout_ms);
int  net_recv_from(net_state_t *n, int ch_idx, packet_t *pkt, uint32_t timeout_ms);
void net_broadcast(net_state_t *n, packet_t *pkt, uint32_t timeout_ms);
int  net_send_to_node(net_state_t *n, uint32_t dest_node_id, packet_t *pkt, uint32_t timeout_ms);
void net_poll(net_state_t *n);
int  net_add_known_peer(net_state_t *n, const uint8_t *bd_addr, uint32_t node_id, int channel_idx);
int  net_find_known_peer(net_state_t *n, uint32_t node_id);

static inline uint32_t node_id_from_bdaddr(const uint8_t *addr) {
    return crc32(addr, 6);
}

static inline uint8_t packet_checksum(packet_t *pkt) {
    uint8_t sum = pkt->preamble ^ pkt->len ^ pkt->type;
    for(int i = 0; i < pkt->len; i++)
        sum ^= pkt->payload[i];
    return sum;
}

static inline void packet_build(packet_t *pkt, uint8_t type,
                                 const void *payload, uint8_t len) {
    pkt->preamble = PREAMBLE;
    pkt->type = type;
    pkt->len = len;
    if(payload && len > 0) {
        const uint8_t *src = payload;
        for(int i = 0; i < len; i++)
            pkt->payload[i] = src[i];
    }
    pkt->checksum = packet_checksum(pkt);
}

static inline int packet_verify(packet_t *pkt) {
    if(pkt->preamble != PREAMBLE) return 0;
    if(pkt->len > MAX_PAYLOAD) return 0;
    return packet_checksum(pkt) == pkt->checksum;
}

static inline int ch_queue_empty(channel_t *ch) {
    return ch->q_head == ch->q_tail;
}
static inline int ch_queue_full(channel_t *ch) {
    return ((ch->q_tail + 1) % PKT_QUEUE_SIZE) == ch->q_head;
}

#endif
