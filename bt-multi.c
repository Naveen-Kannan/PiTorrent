#include "bt-multi.h"

static uint32_t uart_read32_le(void) {
    uint32_t v = (uint8_t)uart_get8();
    v |= ((uint32_t)(uint8_t)uart_get8()) << 8;
    v |= ((uint32_t)(uint8_t)uart_get8()) << 16;
    v |= ((uint32_t)(uint8_t)uart_get8()) << 24;
    return v;
}

static uint16_t uart_read16_le(void) {
    uint16_t v = (uint8_t)uart_get8();
    v |= ((uint16_t)(uint8_t)uart_get8()) << 8;
    return v;
}

void bt_multi_init_from_mac(bt_multi_state_t *bt, net_state_t *net) {
    bt->net = net;
    bt->self_id = net->node_id;
    bt->dht = 0;
    bt->is_seed = 0;
    bt->chunks_done = 0;

    for(int i = 0; i < NUM_CHUNKS; i++) {
        bt->have[i] = 0;
        bt->chunk_peer[i] = -1;
    }
    for(int i = 0; i < MAX_CHANNELS; i++) {
        bt->peer_has_bitfield[i] = 0;
        bt->sent_bitfield[i] = 0;
        for(int j = 0; j < NUM_CHUNKS; j++)
            bt->peer_have[i][j] = 0;
    }
    for(int i = 0; i < FILE_SIZE; i++)
        bt->data[i] = 0;

    printk("bt: waiting for chunks from Mac...\n");

    int first_byte = -1;
    uint32_t last_aa = 0;
    while(first_byte < 0) {
        uint32_t now = timer_get_usec();
        if(now - last_aa > 500000) {
            uart_put8(0xAA);
            last_aa = now;
        }
        first_byte = uart_get8_async();
        if(first_byte < 0) delay_us(500);
    }

    bt->file_id = (uint8_t)first_byte;
    bt->file_id |= ((uint32_t)(uint8_t)uart_get8()) << 8;
    bt->file_id |= ((uint32_t)(uint8_t)uart_get8()) << 16;
    bt->file_id |= ((uint32_t)(uint8_t)uart_get8()) << 24;

    uint16_t nc = uart_read16_le();
    uint16_t cs = uart_read16_le();

    int header_ok = (nc == NUM_CHUNKS && cs == CHUNK_SIZE);

    if(header_ok) {
        for(int i = 0; i < NUM_CHUNKS; i++)
            bt->chunk_hashes[i] = uart_read32_le();

        uint8_t num_assigned = (uint8_t)uart_get8();

        for(int i = 0; i < num_assigned; i++) {
            uint8_t idx = (uint8_t)uart_get8();
            if(idx < NUM_CHUNKS) {
                for(int j = 0; j < CHUNK_SIZE; j++)
                    bt->data[idx * CHUNK_SIZE + j] = (uint8_t)uart_get8();
                bt->have[idx] = 1;
                bt->chunks_done++;
            } else {
                for(int j = 0; j < CHUNK_SIZE; j++)
                    (void)uart_get8();
            }
        }

        uart_read32_le();
    }

    uart_put8(0x55);

    if(!header_ok) {
        printk("bt: ERROR: expected %d chunks of %d bytes, got %d of %d\n",
               NUM_CHUNKS, CHUNK_SIZE, nc, cs);
        clean_reboot();
    }

    for(int i = 0; i < NUM_CHUNKS; i++) {
        if(!bt->have[i]) continue;
        uint32_t h = crc32(&bt->data[i * CHUNK_SIZE], CHUNK_SIZE);
        if(h != bt->chunk_hashes[i]) {
            printk("bt: chunk %d corrupt! got=%x exp=%x\n", i, h, bt->chunk_hashes[i]);
            bt->have[i] = 0;
            bt->chunks_done--;
        }
    }

    printk("bt: received %d/%d chunks, file_id=%x\n",
           bt->chunks_done, NUM_CHUNKS, bt->file_id);
    printk("bt: have: ");
    for(int i = 0; i < NUM_CHUNKS; i++)
        if(bt->have[i]) printk("%d ", i);
    printk("\n");
    printk("bt: need: ");
    for(int i = 0; i < NUM_CHUNKS; i++)
        if(!bt->have[i]) printk("%d ", i);
    printk("\n");
}

void bt_multi_send_bitfield(bt_multi_state_t *bt, int ch_idx) {
    bitfield_payload_t bf;
    bf.file_id = bt->file_id;
    for(int i = 0; i < NUM_CHUNKS; i++)
        bf.have[i] = bt->have[i];

    packet_t pkt;
    packet_build(&pkt, MSG_BITFIELD, &bf, sizeof(bf));
    if(net_send(bt->net, ch_idx, &pkt, 2000) == NET_OK)
        bt->sent_bitfield[ch_idx] = 1;
}

static void handle_bitfield(bt_multi_state_t *bt, int from_ch, packet_t *pkt) {
    bitfield_payload_t *bf = (bitfield_payload_t *)pkt->payload;

    if(bf->file_id != bt->file_id) return;
    if(bt->peer_has_bitfield[from_ch]) return;

    for(int i = 0; i < NUM_CHUNKS; i++)
        bt->peer_have[from_ch][i] = bf->have[i];
    bt->peer_has_bitfield[from_ch] = 1;

    int peer_count = 0;
    for(int i = 0; i < NUM_CHUNKS; i++)
        if(bf->have[i]) peer_count++;

    printk("bt: got bitfield from ch %d (%d/%d chunks)\n", from_ch, peer_count, NUM_CHUNKS);

    bt_multi_send_bitfield(bt, from_ch);
}

void bt_multi_broadcast_have(bt_multi_state_t *bt, int chunk_num) {
    have_payload_t hp;
    hp.chunk_num = chunk_num;

    packet_t pkt;
    packet_build(&pkt, MSG_HAVE, &hp, sizeof(hp));
    net_broadcast(bt->net, &pkt, 1000);
}

static void handle_have(bt_multi_state_t *bt, int from_ch, packet_t *pkt) {
    have_payload_t *hp = (have_payload_t *)pkt->payload;
    if(hp->chunk_num < NUM_CHUNKS)
        bt->peer_have[from_ch][hp->chunk_num] = 1;
}

static int find_chunk_peer(bt_multi_state_t *bt, int chunk_num) {
    int best_ch = -1;
    int best_count = NUM_CHUNKS + 1;

    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(!bt->net->ch_active[i]) continue;
        if(!bt->peer_has_bitfield[i]) continue;
        if(!bt->peer_have[i][chunk_num]) continue;

        int count = 0;
        for(int j = 0; j < NUM_CHUNKS; j++)
            if(bt->peer_have[i][j]) count++;

        if(count < best_count) {
            best_count = count;
            best_ch = i;
        }
    }

    return best_ch;
}

void bt_multi_announce_chunk(bt_multi_state_t *bt, int chunk_num) {
    if(!bt->dht) return;
    chunk_owner_t owner;
    owner.node_id = bt->self_id;
    owner.chunk_num = chunk_num;
    dht_put(bt->dht, bt->chunk_hashes[chunk_num], &owner, sizeof(owner));
}

void bt_multi_send_chunk(bt_multi_state_t *bt, int peer_ch, int chunk_num) {
    if(!bt->have[chunk_num]) return;

    uint8_t *src = &bt->data[chunk_num * CHUNK_SIZE];
    int sent = 0;
    int frag = 0;

    while(sent < CHUNK_SIZE) {
        bt_chunk_t cp;
        cp.sender_id = bt->self_id;
        cp.chunk_num = chunk_num;
        cp.offset = frag;

        int remaining = CHUNK_SIZE - sent;
        cp.frag_len = (remaining > CHUNK_FRAG_DATA) ? CHUNK_FRAG_DATA : remaining;

        for(int i = 0; i < cp.frag_len; i++)
            cp.data[i] = src[sent + i];

        packet_t pkt;
        packet_build(&pkt, MSG_CHUNK, &cp, 7 + cp.frag_len);
        net_send(bt->net, peer_ch, &pkt, 5000);

        sent += cp.frag_len;
        frag++;
        delay_ms(5);
    }
}

int bt_multi_recv_chunk(bt_multi_state_t *bt, int chunk_num, uint32_t timeout_ms) {
    uint8_t buf[CHUNK_SIZE];
    int received = 0;

    uint32_t start = timer_get_usec();
    while(received < CHUNK_SIZE) {
        uint32_t elapsed = (timer_get_usec() - start) / 1000;
        if(elapsed >= timeout_ms)
            return NET_TIMEOUT;

        int from;
        packet_t pkt;
        uint32_t rem = timeout_ms - elapsed;
        if(rem > 2000) rem = 2000;

        if(net_recv(bt->net, &from, &pkt, rem) == NET_OK) {
            if(pkt.type == MSG_CHUNK) {
                bt_chunk_t *cp = (bt_chunk_t *)pkt.payload;
                if(cp->chunk_num != chunk_num) continue;

                int offset = cp->offset * CHUNK_FRAG_DATA;
                for(int i = 0; i < cp->frag_len; i++) {
                    if(offset + i < CHUNK_SIZE)
                        buf[offset + i] = cp->data[i];
                }
                received += cp->frag_len;
            } else if(pkt.type == MSG_REQUEST) {
                bt_request_t *req = (bt_request_t *)pkt.payload;
                if(bt->have[req->chunk_num])
                    bt_multi_send_chunk(bt, from, req->chunk_num);
            } else if(pkt.type == MSG_BITFIELD) {
                handle_bitfield(bt, from, &pkt);
            } else if(pkt.type == MSG_HAVE) {
                handle_have(bt, from, &pkt);
            } else {
                if(bt->dht)
                    dht_handle_msg(bt->dht, from, &pkt);
            }
        }
    }

    uint32_t hash = crc32(buf, CHUNK_SIZE);
    if(hash != bt->chunk_hashes[chunk_num]) {
        printk("bt: chunk %d hash mismatch! got %x expected %x\n",
               chunk_num, hash, bt->chunk_hashes[chunk_num]);
        return NET_BAD_CHECKSUM;
    }

    for(int i = 0; i < CHUNK_SIZE; i++)
        bt->data[chunk_num * CHUNK_SIZE + i] = buf[i];
    bt->have[chunk_num] = 1;
    bt->chunks_done++;

    return NET_OK;
}

int bt_multi_tick(bt_multi_state_t *bt) {
    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(!bt->net->ch_active[i] && (bt->peer_has_bitfield[i] || bt->sent_bitfield[i])) {
            bt->peer_has_bitfield[i] = 0;
            bt->sent_bitfield[i] = 0;
            for(int j = 0; j < NUM_CHUNKS; j++)
                bt->peer_have[i][j] = 0;
        }
    }

    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(bt->net->ch_active[i] && !bt->sent_bitfield[i]) {
            bt_multi_send_bitfield(bt, i);
        }
    }

    int from;
    packet_t pkt;
    while(net_recv(bt->net, &from, &pkt, 100) == NET_OK) {
        if(pkt.type == MSG_REQUEST) {
            bt_request_t *req = (bt_request_t *)pkt.payload;
            if(bt->have[req->chunk_num]) {
                printk("bt: serving chunk %d to ch %d\n", req->chunk_num, from);
                bt_multi_send_chunk(bt, from, req->chunk_num);
            }
        } else if(pkt.type == MSG_BITFIELD) {
            handle_bitfield(bt, from, &pkt);
        } else if(pkt.type == MSG_HAVE) {
            handle_have(bt, from, &pkt);
        } else {
            if(bt->dht)
                dht_handle_msg(bt->dht, from, &pkt);
        }
    }

    if(bt->chunks_done >= NUM_CHUNKS)
        return 1;

    int next = -1;
    int peer_ch = -1;
    for(int i = 0; i < NUM_CHUNKS; i++) {
        if(bt->have[i]) continue;
        int ch = find_chunk_peer(bt, i);
        if(ch >= 0) {
            next = i;
            peer_ch = ch;
            break;
        }
    }

    if(next < 0 || peer_ch < 0)
        return 0;

    printk("bt: requesting chunk %d from ch %d\n", next, peer_ch);
    bt_request_t req;
    req.sender_id = bt->self_id;
    req.chunk_num = next;

    packet_build(&pkt, MSG_REQUEST, &req, sizeof(req));
    net_send(bt->net, peer_ch, &pkt, 5000);

    int rc = bt_multi_recv_chunk(bt, next, 10000);
    if(rc == NET_OK) {
        printk("bt: chunk %d OK (%d/%d)\n", next, bt->chunks_done, NUM_CHUNKS);
        bt_multi_broadcast_have(bt, next);
        if(bt->dht)
            bt_multi_announce_chunk(bt, next);
    } else {
        printk("bt: chunk %d failed (%d), will retry\n", next, rc);
    }

    return 0;
}
