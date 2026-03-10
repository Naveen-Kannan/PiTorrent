#include "net.h"
#include "hci-consts.h"
#include "gpio-high.h"
#include <string.h>

static int packet_serialize(packet_t *pkt, uint8_t *buf) {
    int idx = 0;
    buf[idx++] = pkt->preamble;
    buf[idx++] = pkt->len;
    buf[idx++] = pkt->type;
    for(int i = 0; i < pkt->len; i++)
        buf[idx++] = pkt->payload[i];
    buf[idx++] = pkt->checksum;
    return idx;
}

static int packet_deserialize(const uint8_t *buf, int buf_len, packet_t *pkt) {
    if(buf_len < 4) return 0;
    pkt->preamble = buf[0];
    pkt->len = buf[1];
    pkt->type = buf[2];
    if(buf_len < 3 + pkt->len + 1) return 0;
    for(int i = 0; i < pkt->len; i++)
        pkt->payload[i] = buf[3 + i];
    pkt->checksum = buf[3 + pkt->len];
    return 1;
}

static int find_channel_by_handle(net_state_t *n, uint16_t handle) {
    handle &= 0x0FFF;
    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(n->ch[i].connected && (n->ch[i].acl_handle & 0x0FFF) == handle)
            return i;
    }
    return -1;
}

static int bdaddr_cmp(const uint8_t *a, const uint8_t *b) {
    for(int i = 5; i >= 0; i--) {
        if(a[i] < b[i]) return -1;
        if(a[i] > b[i]) return 1;
    }
    return 0;
}

static void print_bdaddr(const uint8_t *addr) {
    for(int i = 5; i >= 0; i--)
        printk("%x%s", addr[i], i ? ":" : "");
}

static int is_rpi_bdaddr(const uint8_t *addr_le) {
    return addr_le[5] == 0xb8 && addr_le[4] == 0x27 && addr_le[3] == 0xeb;
}

static int find_free_channel(net_state_t *n) {
    for(int i = 0; i < MAX_CHANNELS; i++)
        if(!n->ch_active[i])
            return i;
    return -1;
}

static int has_connection_to(net_state_t *n, const uint8_t *bd_addr) {
    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(n->ch_active[i] && memcmp(n->ch[i].bd_addr, bd_addr, 6) == 0)
            return i;
    }
    return -1;
}

int net_add_known_peer(net_state_t *n, const uint8_t *bd_addr, uint32_t node_id, int channel_idx) {
    for(int i = 0; i < n->num_known; i++) {
        if(n->known[i].valid && n->known[i].node_id == node_id) {
            if(channel_idx >= 0)
                n->known[i].channel_idx = channel_idx;
            return i;
        }
    }

    if(n->num_known < MAX_KNOWN_PEERS) {
        int idx = n->num_known++;
        memcpy(n->known[idx].bd_addr, bd_addr, 6);
        n->known[idx].node_id = node_id;
        n->known[idx].channel_idx = channel_idx;
        n->known[idx].next_hop_ch = -1;
        n->known[idx].valid = 1;
        return idx;
    }

    for(int i = 0; i < MAX_KNOWN_PEERS; i++) {
        if(!n->known[i].valid) {
            memcpy(n->known[i].bd_addr, bd_addr, 6);
            n->known[i].node_id = node_id;
            n->known[i].channel_idx = channel_idx;
            n->known[i].next_hop_ch = -1;
            n->known[i].valid = 1;
            return i;
        }
    }
    return -1;
}

int net_find_known_peer(net_state_t *n, uint32_t node_id) {
    for(int i = 0; i < n->num_known; i++)
        if(n->known[i].valid && n->known[i].node_id == node_id)
            return i;
    return -1;
}

void net_init(net_state_t *n) {
    memset(n, 0, sizeof(*n));

    for(int i = 0; i < MAX_CHANNELS; i++) {
        n->ch[i].connected = 0;
        n->ch[i].q_head = 0;
        n->ch[i].q_tail = 0;
    }

    for(int i = 0; i < MAX_KNOWN_PEERS; i++)
        n->known[i].valid = 0;

    n->new_connection = -1;
    n->gossip_cursor = 0;

    bt_init();
    bt_upload_firmware();

    struct hci_command_pkt cmd = {0};
    cmd.opcode = CMD_READ_BD_ADDR;
    bt_send_command(&cmd);

    struct hci_event_pkt *evt = bt_receive_event();
    assert(evt->event_code == EVENT_COMMAND_COMPLETE);
    assert(evt->params[3] == 0);
    memcpy(n->self_addr, &evt->params[4], 6);

    n->node_id = node_id_from_bdaddr(n->self_addr);

    printk("bt-net: BD_ADDR = ");
    print_bdaddr(n->self_addr);
    printk("  node_id = %x\n", n->node_id);
}

void net_int_init(net_state_t *n) {
    (void)n;
}

static int register_connection(net_state_t *n, struct hci_event_pkt *evt) {
    if(evt->params[0] != 0) return -1;

    int ch = find_free_channel(n);
    if(ch < 0) return -1;

    n->ch[ch].acl_handle = evt->params[1] | (evt->params[2] << 8);
    memcpy(n->ch[ch].bd_addr, &evt->params[3], 6);
    n->ch[ch].connected = 1;
    n->ch[ch].q_head = 0;
    n->ch[ch].q_tail = 0;
    n->ch_active[ch] = 1;
    n->num_active++;
    n->new_connection = ch;

    uint32_t peer_id = node_id_from_bdaddr(n->ch[ch].bd_addr);
    net_add_known_peer(n, n->ch[ch].bd_addr, peer_id, ch);

    printk("bt-net: connected ch %d handle=%x to ", ch, n->ch[ch].acl_handle);
    print_bdaddr(n->ch[ch].bd_addr);
    printk(" (node %x)\n", peer_id);

    return ch;
}

static int bt_create_connection(net_state_t *n, uint8_t *addr_le, uint32_t timeout_ms) {
    if(has_connection_to(n, addr_le) >= 0) return 1;
    if(n->num_active >= MAX_CHANNELS) return 0;

    struct hci_command_pkt cmd = {0};
    struct hci_event_pkt *evt;

    cmd.opcode = CMD_CREATE_CONNECTION;
    cmd.params_len = 13;
    memcpy(cmd.params, addr_le, 6);
    cmd.params[6] = 0x18; cmd.params[7] = 0xcc;
    cmd.params[8] = 0x01;
    cmd.params[9] = 0x00;
    cmd.params[10] = 0x00; cmd.params[11] = 0x00;
    cmd.params[12] = 0x00;
    bt_send_command(&cmd);

    printk("bt-net: connecting to ");
    print_bdaddr(addr_le);
    printk("...\n");

    uint32_t start = timer_get_usec();
    while(1) {
        evt = bt_receive_event_async();
        if(evt) {
            if(evt->event_code == EVENT_COMMAND_STATUS) {
                u16 op = evt->params[2] | (evt->params[3] << 8);
                if(op == CMD_CREATE_CONNECTION) {
                    if(evt->params[0] != 0) {
                        printk("bt-net: create_connection status=%x\n", evt->params[0]);
                        return 0;
                    }
                    break;
                }
            }
            if(evt->event_code == EVENT_CONNECTION_REQUEST && n->num_active < MAX_CHANNELS) {
                struct hci_command_pkt acmd = {0};
                acmd.opcode = CMD_ACCEPT_CONNECTION_REQUEST;
                acmd.params_len = 7;
                memcpy(acmd.params, evt->params, 6);
                acmd.params[6] = 0x01;
                bt_send_command(&acmd);
            }
            if(evt->event_code == EVENT_CONNECTION_COMPLETE)
                register_connection(n, evt);
        }
        if((timer_get_usec() - start) / 1000 >= timeout_ms) return 0;
    }

    while(1) {
        evt = bt_receive_event_async();
        if(evt) {
            if(evt->event_code == EVENT_CONNECTION_COMPLETE) {
                if(memcmp(&evt->params[3], addr_le, 6) == 0)
                    return (register_connection(n, evt) >= 0) ? 1 : 0;
                register_connection(n, evt);
            }
            if(evt->event_code == EVENT_CONNECTION_REQUEST && n->num_active < MAX_CHANNELS) {
                struct hci_command_pkt acmd = {0};
                acmd.opcode = CMD_ACCEPT_CONNECTION_REQUEST;
                acmd.params_len = 7;
                memcpy(acmd.params, evt->params, 6);
                acmd.params[6] = 0x01;
                bt_send_command(&acmd);
            }
        }
        if((timer_get_usec() - start) / 1000 >= timeout_ms) {
            printk("bt-net: connection timeout\n");
            return 0;
        }
    }
}

static int bt_inquiry_short(uint8_t discovered[][6], int max_devices) {
    struct hci_command_pkt cmd = {0};

    cmd.opcode = CMD_INQUIRY;
    cmd.params_len = 5;
    cmd.params[0] = 0x33;
    cmd.params[1] = 0x8B;
    cmd.params[2] = 0x9E;
    cmd.params[3] = SHORT_INQUIRY_LEN;
    cmd.params[4] = 0;
    bt_send_command(&cmd);

    struct hci_event_pkt *evt = bt_receive_event();
    if(evt->event_code != EVENT_COMMAND_STATUS || evt->params[0] != 0)
        return 0;

    int count = 0;

    while(1) {
        evt = bt_receive_event();
        if(!evt) continue;

        if(evt->event_code == EVENT_INQUIRY_RESULT ||
           evt->event_code == EVENT_INQUIRY_RESULT_WITH_RSSI) {
            int num = evt->params[0];
            for(int i = 0; i < num && count < max_devices; i++) {
                uint8_t *addr = &evt->params[1 + i * 14];
                if(!is_rpi_bdaddr(addr)) continue;
                int dup = 0;
                for(int j = 0; j < count; j++)
                    if(memcmp(discovered[j], addr, 6) == 0) { dup = 1; break; }
                if(!dup) {
                    memcpy(discovered[count], addr, 6);
                    count++;
                }
            }
        } else if(evt->event_code == EVENT_EXTENDED_INQUIRY_RESULT) {
            uint8_t *addr = &evt->params[1];
            if(is_rpi_bdaddr(addr)) {
                int dup = 0;
                for(int j = 0; j < count; j++)
                    if(memcmp(discovered[j], addr, 6) == 0) { dup = 1; break; }
                if(!dup && count < max_devices) {
                    memcpy(discovered[count], addr, 6);
                    count++;
                }
            }
        } else if(evt->event_code == EVENT_INQUIRY_COMPLETE) {
            break;
        }
    }

    return count;
}

void net_start_discovery(net_state_t *n) {
    struct hci_command_pkt cmd = {0};
    struct hci_event_pkt *evt;

    cmd.opcode = CMD_WRITE_SCAN_ENABLE;
    cmd.params_len = 1;
    cmd.params[0] = 0x03;
    bt_send_command(&cmd);
    evt = bt_receive_event();
    (void)evt;

    n->discovery_active = 1;
    n->last_inquiry_us = 0;
    n->last_gossip_us = timer_get_usec();

    printk("bt-net: discovery started (inquiry + page scan)\n");
}

int net_discovery_tick(net_state_t *n) {
    if(!n->discovery_active) return 0;

    net_poll(n);

    int new_conn = 0;
    if(n->new_connection >= 0) {
        new_conn = 1;
        n->new_connection = -1;
    }

    if(n->num_active >= MAX_PEERS) {
        return new_conn;
    }

    uint32_t now = timer_get_usec();
    uint32_t since_last = (now - n->last_inquiry_us) / 1000;

    if(n->last_inquiry_us == 0 || since_last > INQUIRY_INTERVAL_MS) {
        printk("bt-net: running short inquiry...\n");

        uint8_t discovered[MAX_DISCOVERED][6];
        int found = bt_inquiry_short(discovered, MAX_DISCOVERED);

        n->last_inquiry_us = timer_get_usec();

        printk("bt-net: inquiry found %d device(s)\n", found);

        for(int i = 0; i < found && n->num_active < MAX_PEERS; i++) {
            if(memcmp(discovered[i], n->self_addr, 6) == 0) continue;
            if(has_connection_to(n, discovered[i]) >= 0) continue;

            printk("bt-net: connecting to discovered peer ");
            print_bdaddr(discovered[i]);
            printk("\n");

            int ok = 0;
            for(int attempt = 0; attempt < 2 && !ok; attempt++) {
                if(bt_create_connection(n, discovered[i], 10000))
                    ok = 1;
                else
                    delay_ms(500);
            }
        }
    }

    if(n->new_connection >= 0) {
        new_conn = 1;
        n->new_connection = -1;
    }

    return new_conn;
}

static void send_peer_list(net_state_t *n, int ch_idx) {
    peer_list_payload_t pl;
    pl.count = 0;

    memcpy(pl.peers[pl.count].bd_addr, n->self_addr, 6);
    pl.peers[pl.count].node_id = n->node_id;
    pl.count++;

    for(int i = 0; i < n->num_known && pl.count < MAX_KNOWN_PEERS; i++) {
        if(!n->known[i].valid) continue;
        memcpy(pl.peers[pl.count].bd_addr, n->known[i].bd_addr, 6);
        pl.peers[pl.count].node_id = n->known[i].node_id;
        pl.count++;
    }

    packet_t pkt;
    int payload_len = 1 + pl.count * sizeof(peer_entry_t);
    packet_build(&pkt, MSG_PEER_LIST, &pl, payload_len);
    net_send(n, ch_idx, &pkt, 2000);
}

static void handle_peer_list(net_state_t *n, int from_ch, packet_t *pkt) {
    peer_list_payload_t *pl = (peer_list_payload_t *)pkt->payload;

    for(int i = 0; i < pl->count; i++) {
        uint32_t nid = pl->peers[i].node_id;
        if(nid == n->node_id) continue;

        int existing = net_find_known_peer(n, nid);
        if(existing < 0) {
            int idx = net_add_known_peer(n, pl->peers[i].bd_addr, nid, -1);
            if(idx >= 0) {
                n->known[idx].next_hop_ch = from_ch;
                printk("bt-net: gossip learned about node %x via ch %d\n", nid, from_ch);
            }
        } else if(n->known[existing].channel_idx < 0) {
            n->known[existing].next_hop_ch = from_ch;
        }
    }

    if(pkt->type == MSG_PEER_LIST) {
        peer_list_payload_t rpl;
        rpl.count = 0;

        memcpy(rpl.peers[rpl.count].bd_addr, n->self_addr, 6);
        rpl.peers[rpl.count].node_id = n->node_id;
        rpl.count++;

        for(int i = 0; i < n->num_known && rpl.count < MAX_KNOWN_PEERS; i++) {
            if(!n->known[i].valid) continue;
            memcpy(rpl.peers[rpl.count].bd_addr, n->known[i].bd_addr, 6);
            rpl.peers[rpl.count].node_id = n->known[i].node_id;
            rpl.count++;
        }

        packet_t rpkt;
        int rlen = 1 + rpl.count * sizeof(peer_entry_t);
        packet_build(&rpkt, MSG_PEER_LIST_RESP, &rpl, rlen);
        net_send(n, from_ch, &rpkt, 2000);
    }
}

void net_gossip_tick(net_state_t *n) {
    if(n->num_active == 0) return;

    uint32_t now = timer_get_usec();
    uint32_t since = (now - n->last_gossip_us) / 1000;
    if(since < 10000) return;

    n->last_gossip_us = now;

    int start = n->gossip_cursor;
    for(int i = 0; i < MAX_CHANNELS; i++) {
        int idx = (start + i) % MAX_CHANNELS;
        if(n->ch_active[idx]) {
            send_peer_list(n, idx);
            n->gossip_cursor = (idx + 1) % MAX_CHANNELS;

            if(n->num_active < MAX_PEERS) {
                for(int j = 0; j < n->num_known; j++) {
                    if(!n->known[j].valid) continue;
                    if(n->known[j].channel_idx >= 0) continue;
                    if(n->known[j].node_id == n->node_id) continue;
                    if(has_connection_to(n, n->known[j].bd_addr) >= 0) continue;

                    printk("bt-net: gossip-connecting to node %x\n", n->known[j].node_id);
                    if(bt_create_connection(n, n->known[j].bd_addr, 8000)) break;
                }
            }
            break;
        }
    }
}

static void handle_forward(net_state_t *n, int from_ch, packet_t *pkt) {
    forward_t *fwd = (forward_t *)pkt->payload;

    if(fwd->dest_id == n->node_id) {
        packet_t inner;
        inner.preamble = PREAMBLE;
        inner.type = fwd->inner_type;
        inner.len = fwd->inner_len;
        for(int i = 0; i < fwd->inner_len; i++)
            inner.payload[i] = fwd->inner_payload[i];
        inner.checksum = packet_checksum(&inner);

        if(!ch_queue_full(&n->ch[from_ch])) {
            n->ch[from_ch].queue[n->ch[from_ch].q_tail] = inner;
            n->ch[from_ch].q_tail = (n->ch[from_ch].q_tail + 1) % PKT_QUEUE_SIZE;
        }
        return;
    }

    if(fwd->ttl == 0) return;
    fwd->ttl--;

    int next = -1;
    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(!n->ch_active[i] || i == from_ch) continue;
        uint32_t ch_nid = node_id_from_bdaddr(n->ch[i].bd_addr);
        if(ch_nid == fwd->dest_id) {
            next = i;
            break;
        }
    }

    if(next < 0) {
        for(int i = 0; i < n->num_known; i++) {
            if(!n->known[i].valid) continue;
            if(n->known[i].channel_idx >= 0 && n->known[i].channel_idx != from_ch) {
                next = n->known[i].channel_idx;
                break;
            }
        }
    }

    if(next >= 0) {
        pkt->checksum = packet_checksum(pkt);
        net_send(n, next, pkt, 1000);
    }
}

int net_send_to_node(net_state_t *n, uint32_t dest_node_id, packet_t *pkt, uint32_t timeout_ms) {
    if(dest_node_id == n->node_id) return NET_OK;

    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(n->ch_active[i] && node_id_from_bdaddr(n->ch[i].bd_addr) == dest_node_id)
            return net_send(n, i, pkt, timeout_ms);
    }

    int next_hop = -1;
    for(int i = 0; i < n->num_known; i++) {
        if(!n->known[i].valid || n->known[i].node_id != dest_node_id) continue;
        if(n->known[i].channel_idx >= 0) {
            next_hop = n->known[i].channel_idx;
            break;
        }
        if(n->known[i].next_hop_ch >= 0) {
            next_hop = n->known[i].next_hop_ch;
            break;
        }
    }

    if(next_hop < 0) {
        for(int i = 0; i < MAX_CHANNELS; i++) {
            if(n->ch_active[i]) { next_hop = i; break; }
        }
    }

    if(next_hop < 0) return NET_NOT_READY;

    if(pkt->len > MAX_PAYLOAD - FORWARD_HDR_SIZE)
        return NET_NOT_READY;

    forward_t fwd;
    fwd.dest_id = dest_node_id;
    fwd.src_id = n->node_id;
    fwd.ttl = 3;
    fwd.inner_type = pkt->type;
    fwd.inner_len = pkt->len;
    for(int i = 0; i < pkt->len; i++)
        fwd.inner_payload[i] = pkt->payload[i];

    packet_t fpkt;
    packet_build(&fpkt, MSG_FORWARD, &fwd, FORWARD_HDR_SIZE + pkt->len);
    return net_send(n, next_hop, &fpkt, timeout_ms);
}

void net_poll(net_state_t *n) {
    struct hci_acl_data_pkt *acl;
    while((acl = bt_receive_acl_async()) != NULL) {
        int ch = find_channel_by_handle(n, acl->handle);
        if(ch < 0) continue;

        packet_t pkt;
        if(!packet_deserialize(acl->data, acl->data_len, &pkt) || !packet_verify(&pkt))
            continue;

        if(pkt.type == MSG_FORWARD) {
            handle_forward(n, ch, &pkt);
            continue;
        }
        if(pkt.type == MSG_PEER_LIST || pkt.type == MSG_PEER_LIST_RESP) {
            handle_peer_list(n, ch, &pkt);
            continue;
        }

        if(!ch_queue_full(&n->ch[ch])) {
            n->ch[ch].queue[n->ch[ch].q_tail] = pkt;
            n->ch[ch].q_tail = (n->ch[ch].q_tail + 1) % PKT_QUEUE_SIZE;
        }
    }

    struct hci_event_pkt *evt;
    while((evt = bt_receive_event_async()) != NULL) {
        if(evt->event_code == EVENT_DISCONNECTION_COMPLETE) {
            uint16_t handle = evt->params[1] | (evt->params[2] << 8);
            int ch = find_channel_by_handle(n, handle);
            if(ch >= 0) {
                printk("bt-net: ch %d disconnected\n", ch);
                n->ch[ch].connected = 0;
                n->ch_active[ch] = 0;
                n->num_active--;

                uint32_t nid = node_id_from_bdaddr(n->ch[ch].bd_addr);
                int ki = net_find_known_peer(n, nid);
                if(ki >= 0)
                    n->known[ki].channel_idx = -1;
            }
        } else if(evt->event_code == EVENT_CONNECTION_REQUEST) {
            if(n->num_active < MAX_CHANNELS) {
                struct hci_command_pkt cmd = {0};
                cmd.opcode = CMD_ACCEPT_CONNECTION_REQUEST;
                cmd.params_len = 7;
                memcpy(cmd.params, evt->params, 6);
                cmd.params[6] = 0x01;
                bt_send_command(&cmd);
            }
        } else if(evt->event_code == EVENT_CONNECTION_COMPLETE) {
            register_connection(n, evt);
        }
    }
}

int net_send(net_state_t *n, int ch_idx, packet_t *pkt, uint32_t timeout_ms) {
    (void)timeout_ms;
    if(ch_idx < 0 || ch_idx >= MAX_CHANNELS) return NET_NOT_READY;
    if(!n->ch_active[ch_idx] || !n->ch[ch_idx].connected) return NET_NOT_READY;

    uint8_t buf[MAX_PAYLOAD + 4];
    int len = packet_serialize(pkt, buf);

    struct hci_acl_data_pkt acl = {0};
    acl.handle = n->ch[ch_idx].acl_handle;
    acl.data_len = len;
    memcpy(acl.data, buf, len);
    bt_send_acl_data(&acl);

    return NET_OK;
}

int net_recv(net_state_t *n, int *from_ch, packet_t *pkt, uint32_t timeout_ms) {
    uint32_t start = timer_get_usec();
    while(1) {
        net_poll(n);
        for(int i = 0; i < MAX_CHANNELS; i++) {
            if(!n->ch_active[i]) continue;
            if(!ch_queue_empty(&n->ch[i])) {
                *from_ch = i;
                *pkt = n->ch[i].queue[n->ch[i].q_head];
                n->ch[i].q_head = (n->ch[i].q_head + 1) % PKT_QUEUE_SIZE;
                return NET_OK;
            }
        }
        if((timer_get_usec() - start) / 1000 >= timeout_ms)
            return NET_TIMEOUT;
    }
}

int net_recv_from(net_state_t *n, int ch_idx, packet_t *pkt, uint32_t timeout_ms) {
    uint32_t start = timer_get_usec();
    while(1) {
        net_poll(n);
        if(n->ch_active[ch_idx] && !ch_queue_empty(&n->ch[ch_idx])) {
            *pkt = n->ch[ch_idx].queue[n->ch[ch_idx].q_head];
            n->ch[ch_idx].q_head = (n->ch[ch_idx].q_head + 1) % PKT_QUEUE_SIZE;
            return NET_OK;
        }
        if((timer_get_usec() - start) / 1000 >= timeout_ms)
            return NET_TIMEOUT;
    }
}

void net_broadcast(net_state_t *n, packet_t *pkt, uint32_t timeout_ms) {
    for(int i = 0; i < MAX_CHANNELS; i++)
        if(n->ch_active[i])
            net_send(n, i, pkt, timeout_ms);
}
