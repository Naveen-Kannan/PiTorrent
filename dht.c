#include "dht.h"

void dht_init(dht_state_t *dht, uint32_t self_id, net_state_t *net) {
    dht->self_id = self_id;
    dht->net = net;
    dht->store_count = 0;
    rt_init(&dht->rt, self_id);

    for(int i = 0; i < MAX_KV_ENTRIES; i++)
        dht->store[i].valid = 0;
}

void dht_add_peer(dht_state_t *dht, uint32_t peer_id, int channel_idx) {
    rt_add(&dht->rt, peer_id, channel_idx);
}

int dht_local_store(dht_state_t *dht, uint32_t key, const void *value, int len) {
    if(len > MAX_VALUE_LEN) return -1;

    for(int i = 0; i < MAX_KV_ENTRIES; i++) {
        if(dht->store[i].valid && dht->store[i].key == key) {
            const uint8_t *src = value;
            for(int j = 0; j < len; j++)
                dht->store[i].value[j] = src[j];
            dht->store[i].value_len = len;
            return 0;
        }
    }

    for(int i = 0; i < MAX_KV_ENTRIES; i++) {
        if(!dht->store[i].valid) {
            dht->store[i].key = key;
            const uint8_t *src = value;
            for(int j = 0; j < len; j++)
                dht->store[i].value[j] = src[j];
            dht->store[i].value_len = len;
            dht->store[i].valid = 1;
            dht->store_count++;
            return 0;
        }
    }
    return -1;
}

int dht_local_get(dht_state_t *dht, uint32_t key, void *buf, int *len) {
    for(int i = 0; i < MAX_KV_ENTRIES; i++) {
        if(dht->store[i].valid && dht->store[i].key == key) {
            uint8_t *dst = buf;
            for(int j = 0; j < dht->store[i].value_len; j++)
                dst[j] = dht->store[i].value[j];
            *len = dht->store[i].value_len;
            return 0;
        }
    }
    return -1;
}

static void handle_find_node(dht_state_t *dht, int from_peer, find_node_req_t *req) {
    rt_add(&dht->rt, req->sender_id, from_peer);

    find_node_resp_t resp;
    resp.sender_id = dht->self_id;
    resp.target_id = req->target_id;

    uint32_t ids[MAX_FIND_RESULTS];
    int chs[MAX_FIND_RESULTS];
    int count = rt_find_closest(&dht->rt, req->target_id, ids, chs, MAX_FIND_RESULTS);

    resp.count = count;
    for(int i = 0; i < count; i++) {
        resp.nodes[i].node_id = ids[i];
        resp.nodes[i].channel_hint = (uint8_t)chs[i];
    }

    packet_t pkt;
    int payload_len = 9 + count * sizeof(node_info_t);
    packet_build(&pkt, MSG_FIND_NODE_RESP, &resp, payload_len);
    net_send_to_node(dht->net, req->sender_id, &pkt, 2000);
}

static void handle_find_node_resp(dht_state_t *dht, int from_peer, find_node_resp_t *resp) {
    rt_add(&dht->rt, resp->sender_id, from_peer);

    for(int i = 0; i < resp->count; i++) {
        if(resp->nodes[i].node_id == dht->self_id) continue;
        int ch = rt_get_channel(&dht->rt, resp->nodes[i].node_id);
        if(ch < 0) ch = from_peer;
        rt_add(&dht->rt, resp->nodes[i].node_id, ch);
    }
}

static void handle_store(dht_state_t *dht, int from_peer, store_req_t *req) {
    rt_add(&dht->rt, req->sender_id, from_peer);

    int rc = dht_local_store(dht, req->key, req->value, req->value_len);

    store_ack_t ack;
    ack.sender_id = dht->self_id;
    ack.key = req->key;
    ack.status = (rc == 0) ? 0 : 1;

    packet_t pkt;
    packet_build(&pkt, MSG_STORE_ACK, &ack, sizeof(ack));
    net_send_to_node(dht->net, req->sender_id, &pkt, 2000);
}

static void handle_find_value(dht_state_t *dht, int from_peer, find_value_req_t *req) {
    rt_add(&dht->rt, req->sender_id, from_peer);

    find_value_resp_t resp;
    resp.sender_id = dht->self_id;
    resp.key = req->key;

    int vlen;
    if(dht_local_get(dht, req->key, resp.value, &vlen) == 0) {
        resp.found = 1;
        resp.value_len = vlen;
        resp.node_count = 0;

        packet_t pkt;
        int plen = 10 + vlen;
        packet_build(&pkt, MSG_FIND_VALUE_RESP, &resp, plen);
        net_send_to_node(dht->net, req->sender_id, &pkt, 2000);
    } else {
        resp.found = 0;
        resp.value_len = 0;

        uint32_t ids[MAX_FIND_RESULTS];
        int chs[MAX_FIND_RESULTS];
        int count = rt_find_closest(&dht->rt, req->key, ids, chs, MAX_FIND_RESULTS);
        resp.node_count = count;
        for(int i = 0; i < count; i++) {
            resp.nodes[i].node_id = ids[i];
            resp.nodes[i].channel_hint = (uint8_t)chs[i];
        }

        packet_t pkt;
        int plen = 10 + count * sizeof(node_info_t);
        packet_build(&pkt, MSG_FIND_VALUE_RESP, &resp, plen);
        net_send_to_node(dht->net, req->sender_id, &pkt, 2000);
    }
}

void dht_handle_msg(dht_state_t *dht, int from_peer, packet_t *pkt) {
    switch(pkt->type) {
        case MSG_FIND_NODE:
            handle_find_node(dht, from_peer, (find_node_req_t *)pkt->payload);
            break;
        case MSG_FIND_NODE_RESP:
            handle_find_node_resp(dht, from_peer, (find_node_resp_t *)pkt->payload);
            break;
        case MSG_STORE:
            handle_store(dht, from_peer, (store_req_t *)pkt->payload);
            break;
        case MSG_STORE_ACK:
            break;
        case MSG_FIND_VALUE:
            handle_find_value(dht, from_peer, (find_value_req_t *)pkt->payload);
            break;
        case MSG_FIND_VALUE_RESP:
            break;
    }
}

typedef struct {
    uint32_t node_id;
} __attribute__((packed)) announce_t;

void dht_bootstrap(dht_state_t *dht) {
    printk("dht: bootstrapping node %x (%d peers)\n",
           dht->self_id, dht->net->num_active);

    announce_t ann = { .node_id = dht->self_id };
    packet_t pkt;
    packet_build(&pkt, MSG_ANNOUNCE, &ann, sizeof(ann));
    net_broadcast(dht->net, &pkt, 2000);

    int found = 0;
    uint32_t start = timer_get_usec();
    while((timer_get_usec() - start) / 1000 < 2000) {
        int from;
        if(net_recv(dht->net, &from, &pkt, 300) == NET_OK) {
            if(pkt.type == MSG_ANNOUNCE) {
                announce_t *a = (announce_t *)pkt.payload;
                dht_add_peer(dht, a->node_id, from);

                announce_t reply = { .node_id = dht->self_id };
                packet_t rpkt;
                packet_build(&rpkt, MSG_ANNOUNCE_ACK, &reply, sizeof(reply));
                net_send(dht->net, from, &rpkt, 2000);
                found++;
            } else if(pkt.type == MSG_ANNOUNCE_ACK) {
                announce_t *a = (announce_t *)pkt.payload;
                dht_add_peer(dht, a->node_id, from);
                found++;
            }
        }
    }

    printk("dht: bootstrap done, found %d peers\n", found);
    rt_print(&dht->rt);
}

static int do_find_node(dht_state_t *dht, int channel, uint32_t target_id,
                        find_node_resp_t *resp) {
    find_node_req_t req;
    req.sender_id = dht->self_id;
    req.target_id = target_id;

    packet_t pkt;
    packet_build(&pkt, MSG_FIND_NODE, &req, sizeof(req));
    if(net_send(dht->net, channel, &pkt, 2000) != NET_OK)
        return -1;

    uint32_t start = timer_get_usec();
    while((timer_get_usec() - start) / 1000 < 3000) {
        int from;
        if(net_recv(dht->net, &from, &pkt, 500) == NET_OK) {
            if(pkt.type == MSG_FIND_NODE_RESP) {
                find_node_resp_t *r = (find_node_resp_t *)pkt.payload;
                *resp = *r;
                handle_find_node_resp(dht, from, r);
                return 0;
            }
            dht_handle_msg(dht, from, &pkt);
        }
    }
    return -1;
}

static int iterative_find_node(dht_state_t *dht, uint32_t target_id,
                               uint32_t *result_ids, int *result_channels, int max) {
    int count = rt_find_closest(&dht->rt, target_id, result_ids, result_channels, max);

    uint32_t queried[16];
    int num_queried = 0;

    for(int round = 0; round < 3; round++) {
        int improved = 0;
        for(int i = 0; i < count && i < max; i++) {
            int skip = 0;
            for(int q = 0; q < num_queried; q++) {
                if(queried[q] == result_ids[i]) { skip = 1; break; }
            }
            if(skip) continue;

            int ch = result_channels[i];
            if(ch < 0) continue;

            if(num_queried < 16)
                queried[num_queried++] = result_ids[i];

            find_node_resp_t resp;
            if(do_find_node(dht, ch, target_id, &resp) == 0) {
                int new_count = rt_find_closest(&dht->rt, target_id,
                                                result_ids, result_channels, max);
                if(new_count > count) improved = 1;
                count = new_count;
            }
        }
        if(!improved) break;
    }
    return count;
}

int dht_put(dht_state_t *dht, uint32_t key, const void *value, int len) {
    if(len > MAX_VALUE_LEN) return -1;

    dht_local_store(dht, key, value, len);

    uint32_t ids[MAX_FIND_RESULTS];
    int chs[MAX_FIND_RESULTS];
    int count = iterative_find_node(dht, key, ids, chs, MAX_FIND_RESULTS);

    int acks = 0;
    for(int i = 0; i < count; i++) {
        if(ids[i] == dht->self_id) { acks++; continue; }

        int ch = chs[i];
        if(ch < 0) continue;

        store_req_t req;
        req.sender_id = dht->self_id;
        req.key = key;
        req.value_len = len;
        const uint8_t *src = value;
        for(int j = 0; j < len; j++)
            req.value[j] = src[j];

        packet_t pkt;
        packet_build(&pkt, MSG_STORE, &req, 9 + len);
        net_send_to_node(dht->net, ids[i], &pkt, 2000);

        uint32_t start = timer_get_usec();
        while((timer_get_usec() - start) / 1000 < 2000) {
            int from;
            if(net_recv(dht->net, &from, &pkt, 500) == NET_OK) {
                if(pkt.type == MSG_STORE_ACK) {
                    store_ack_t *a = (store_ack_t *)pkt.payload;
                    if(a->key == key && a->status == 0)
                        acks++;
                    break;
                }
                dht_handle_msg(dht, from, &pkt);
            }
        }
    }

    return (acks > 0) ? 0 : -1;
}

int dht_get(dht_state_t *dht, uint32_t key, void *buf, int *len) {
    if(dht_local_get(dht, key, buf, len) == 0)
        return 0;

    uint32_t ids[MAX_FIND_RESULTS];
    int chs[MAX_FIND_RESULTS];
    int count = rt_find_closest(&dht->rt, key, ids, chs, MAX_FIND_RESULTS);

    uint32_t queried[16];
    int num_queried = 0;

    for(int round = 0; round < 3; round++) {
        for(int i = 0; i < count; i++) {
            if(ids[i] == dht->self_id) continue;

            int skip = 0;
            for(int q = 0; q < num_queried; q++) {
                if(queried[q] == ids[i]) { skip = 1; break; }
            }
            if(skip) continue;

            int ch = chs[i];
            if(ch < 0) continue;

            if(num_queried < 16)
                queried[num_queried++] = ids[i];

            find_value_req_t req;
            req.sender_id = dht->self_id;
            req.key = key;

            packet_t pkt;
            packet_build(&pkt, MSG_FIND_VALUE, &req, sizeof(req));
            net_send_to_node(dht->net, ids[i], &pkt, 2000);

            uint32_t start = timer_get_usec();
            while((timer_get_usec() - start) / 1000 < 3000) {
                int from;
                if(net_recv(dht->net, &from, &pkt, 500) == NET_OK) {
                    if(pkt.type == MSG_FIND_VALUE_RESP) {
                        find_value_resp_t *r = (find_value_resp_t *)pkt.payload;
                        rt_add(&dht->rt, r->sender_id, from);

                        if(r->found) {
                            uint8_t *dst = buf;
                            for(int j = 0; j < r->value_len; j++)
                                dst[j] = r->value[j];
                            *len = r->value_len;
                            return 0;
                        }

                        for(int j = 0; j < r->node_count; j++) {
                            if(r->nodes[j].node_id != dht->self_id) {
                                int nch = rt_get_channel(&dht->rt, r->nodes[j].node_id);
                                if(nch < 0) nch = from;
                                rt_add(&dht->rt, r->nodes[j].node_id, nch);
                            }
                        }
                        count = rt_find_closest(&dht->rt, key, ids, chs, MAX_FIND_RESULTS);
                        break;
                    }
                    dht_handle_msg(dht, from, &pkt);
                }
            }
        }
    }

    return -1;
}
