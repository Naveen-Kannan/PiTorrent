#include "rpi.h"
#include "net.h"
#include "dht.h"
#include "bt-multi.h"

#define EXCHANGE_TIMEOUT_SEC 180

void notmain(void) {
    uart_init();
    printk("\n=== BT BitTorrent Node starting ===\n");

    net_state_t net;
    net_init(&net);
    net_int_init(&net);

    bt_multi_state_t bt;
    bt_multi_init_from_mac(&bt, &net);

    net_start_discovery(&net);

    printk("waiting for first peer...\n");
    while(net.num_active == 0) {
        net_discovery_tick(&net);
    }
    printk("first peer connected!\n");

    dht_state_t dht;
    dht_init(&dht, net.node_id, &net);
    dht_bootstrap(&dht);
    bt.dht = &dht;
    printk("DHT ready (node_id=%x)\n", net.node_id);

    torrent_meta_t meta;
    meta.file_id = bt.file_id;
    meta.num_chunks = NUM_CHUNKS;
    meta.chunk_size = CHUNK_SIZE;
    for(int i = 0; i < NUM_CHUNKS; i++)
        meta.chunk_hashes[i] = bt.chunk_hashes[i];
    dht_put(&dht, bt.file_id, &meta, sizeof(meta));

    for(int i = 0; i < NUM_CHUNKS; i++)
        if(bt.have[i])
            bt_multi_announce_chunk(&bt, i);

    printk("\nstarting exchange...\n");
    uint32_t xstart = timer_get_usec();

    while(bt.chunks_done < NUM_CHUNKS) {
        uint32_t elapsed_s = (timer_get_usec() - xstart) / 1000000;
        if(elapsed_s > EXCHANGE_TIMEOUT_SEC) {
            printk("exchange timeout after %ds!\n", EXCHANGE_TIMEOUT_SEC);
            break;
        }

        net_discovery_tick(&net);
        net_gossip_tick(&net);
        bt_multi_tick(&bt);
    }

    uint32_t hash = crc32(bt.data, FILE_SIZE);
    if(hash == bt.file_id)
        printk("\n*** FILE COMPLETE! CRC32=%x VERIFIED ***\n", hash);
    else
        printk("\n*** FILE CORRUPT! got %x expected %x ***\n", hash, bt.file_id);

    printk("serving remaining requests...\n");
    int idle = 0;
    while(idle < 500) {
        net_discovery_tick(&net);
        net_gossip_tick(&net);
        if(bt_multi_tick(&bt))
            idle++;
        else
            idle = 0;
    }

    static const char hex[] = "0123456789abcdef";
    printk("===FILE_START===\n");
    for(int i = 0; i < FILE_SIZE; i++) {
        uart_put8(hex[bt.data[i] >> 4]);
        uart_put8(hex[bt.data[i] & 0xf]);
        if((i + 1) % 32 == 0) uart_put8('\n');
    }
    printk("===FILE_END===\n");

    printk("=== Node finished ===\n");
    delay_ms(1000);
    clean_reboot();
}
