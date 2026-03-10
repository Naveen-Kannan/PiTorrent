// Javier Garcia Nieto <jgnieto@cs.stanford.edu>. CS340LX Fall 2025.
// adapted for cs140e-26win libpi

#include "bt.h"
#include "hci-consts.h"
#include "rpi.h"
#include "pl011.h"
#include "gpio-high.h"

#include <stdbool.h>

// simple pointer ring buffer (replaces circular.h with CQE_T=void*)
#define PTR_Q_N 64
typedef struct {
    void *buf[PTR_Q_N];
    unsigned head, tail;
} ptr_q_t;

static inline void pq_init(ptr_q_t *q) { q->head = q->tail = 0; }
static inline int  pq_empty(ptr_q_t *q) { return q->head == q->tail; }
static inline int  pq_push(ptr_q_t *q, void *p) {
    unsigned next = (q->head + 1) % PTR_Q_N;
    if(next == q->tail) return 0;
    q->buf[q->head] = p;
    q->head = next;
    return 1;
}
static inline void *pq_pop(ptr_q_t *q) {
    if(pq_empty(q)) return 0;
    void *p = q->buf[q->tail];
    q->tail = (q->tail + 1) % PTR_Q_N;
    return p;
}

static struct {
    ptr_q_t acl_rx_buffer;
    ptr_q_t event_rx_buffer;
    bool initialized;

    // how many commands can we send without receiving events
    unsigned n_commands_can_send;
} module;

void bt_init(void) {
    assert(!module.initialized);

    module.initialized = true;
    module.n_commands_can_send = 1;

    pl011_init();

    pq_init(&module.acl_rx_buffer);
    pq_init(&module.event_rx_buffer);
    kmalloc_init();

    // enable the BT chip
    gpio_hi_set_output(BT_EN);
    gpio_hi_set_on(BT_EN);
    delay_ms(800);
}

/*
 * HCI ACL Data:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |         Handle        | Flags |       Data Total Length       |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                                                               |
 *  |                              Data                             |
 *  |                               ...                             |
 *  |                                                               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
static void _receive_acl_data(struct hci_acl_data_pkt *acl) {
    assert(module.initialized);

    // handle + flags: 2 bytes little-endian
    u8 lo = pl011_get8();
    u8 hi = pl011_get8();
    acl->handle = lo | (hi << 8);

    // data_len: 2 bytes little-endian
    u8 len_lo = pl011_get8();
    u8 len_hi = pl011_get8();
    acl->data_len = len_lo | (len_hi << 8);

    assert(acl->data_len <= 1021);
    for (int i = 0; i < acl->data_len; i++)
        acl->data[i] = pl011_get8();
}

/*
 * HCI Event:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |   Event Code  |   Params Len  |        Event Parameter 0      |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |        Event Parameter 1      | Evnt Param 2  | Evnt Param 3  |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                               ...                             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |      Event Parameter N-1      |       Event Parameter N       |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
static void _receive_event(struct hci_event_pkt *evt) {
    assert(module.initialized);

    // event_code: 1 byte
    evt->event_code = pl011_get8();
    // params_len: 1 byte
    evt->params_len = pl011_get8();
    // params: exactly params_len bytes
    for (int i = 0; i < evt->params_len; i++)
        evt->params[i] = pl011_get8();

    // The EVENT_COMMAND_COMPLETE and EVENT_COMMAND_STATUS events include flow
    // control information that tells us how many commands we can send until
    // we get another one of these events giving us permission to send more.
    if (evt->event_code == EVENT_COMMAND_COMPLETE && evt->params_len >= 1) {
        u8 n_cmds = evt->params[0];
        module.n_commands_can_send = n_cmds;
    } else if (evt->event_code == EVENT_COMMAND_STATUS && evt->params_len >= 2) {
        u8 n_cmds = evt->params[1];
        module.n_commands_can_send = n_cmds;
    }
}

// Blockingly receive a single packet.
static void _receive_packet(void) {
    assert(module.initialized);
    u8 packet_type = pl011_get8();
    switch (packet_type) {
    case HCI_EVENT: {
        struct hci_event_pkt *evt = kmalloc_notzero(sizeof(*evt));
        _receive_event(evt);
        pq_push(&module.event_rx_buffer, evt);
        break;
    }
    case HCI_ACL_DATA: {
        struct hci_acl_data_pkt *acl = kmalloc_notzero(sizeof(*acl));
        _receive_acl_data(acl);
        pq_push(&module.acl_rx_buffer, acl);
        break;
    }
    default:
        panic("Unexpected BT packet type: %x\n", packet_type);
    }
}


/*
 * HCI Command:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |           OpCode              |  Params len   |  Parameter 0  |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                           Parameter 1                         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                               ...                             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |           Parameter N-1       |           Parameter N         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
void bt_send_command(struct hci_command_pkt *cmd) {
    assert(module.initialized);

    // Wait until we are allowed to send a command. Ideally, here we would have
    // a timeout and reset the Bluetooth module if we wait too long.
    while (module.n_commands_can_send == 0) {
        _receive_packet();
    }
    module.n_commands_can_send--;

    // packet type indicator
    pl011_put8(HCI_CMD);
    // opcode: 2 bytes little-endian
    pl011_put8(cmd->opcode & 0xFF);
    pl011_put8((cmd->opcode >> 8) & 0xFF);
    // params length
    pl011_put8(cmd->params_len);
    // params
    for (int i = 0; i < cmd->params_len; i++)
        pl011_put8(cmd->params[i]);
    pl011_flush_tx();
}

/*
 * HCI ACL Data:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |         Handle        | Flags |       Data Total Length       |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                                                               |
 *  |                              Data                             |
 *  |                               ...                             |
 *  |                                                               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
void bt_send_acl_data(struct hci_acl_data_pkt *acl) {
    assert(module.initialized);

    // packet type indicator
    pl011_put8(HCI_ACL_DATA);
    // handle + flags (flags=0): 2 bytes little-endian
    pl011_put8(acl->handle & 0xFF);
    pl011_put8((acl->handle >> 8) & 0x0F);  // top 4 bits = flags = 0
    // data_len: 2 bytes little-endian
    pl011_put8(acl->data_len & 0xFF);
    pl011_put8((acl->data_len >> 8) & 0xFF);
    // data
    for (int i = 0; i < acl->data_len; i++)
        pl011_put8(acl->data[i]);
    pl011_flush_tx();
}

extern unsigned char BCM43430A1_hcd[];
extern unsigned int BCM43430A1_hcd_len;

void bt_upload_firmware(void) {
    assert(module.initialized);

    // Send a reset and a load firmware command
    struct hci_command_pkt cmd = { 0 };
    cmd.opcode = CMD_RESET;
    bt_send_command(&cmd);
    cmd.opcode = CMD_BCM_LOAD_FIRMWARE;
    bt_send_command(&cmd);

    // Wait for command complete for the load firmware command
    while (1) {
        struct hci_event_pkt *event = bt_receive_event();
        if (event->event_code == EVENT_COMMAND_COMPLETE) {
            assert(event->params_len >= 3);
            u16 opcode = event->params[1] | (event->params[2] << 8);
            if (opcode == CMD_BCM_LOAD_FIRMWARE)
                break;
        }
    }

    printk("About to upload firmware...\n");

    delay_ms(50); // same time as linux

    int n_packets_sent = 0;

    // HCD format: each record is [opcode_lo, opcode_hi, param_len, params...]
    unsigned offset = 0;
    while (offset < BCM43430A1_hcd_len) {
        struct hci_command_pkt fw_cmd = { 0 };
        // opcode: 2 bytes little-endian
        fw_cmd.opcode = BCM43430A1_hcd[offset] | (BCM43430A1_hcd[offset + 1] << 8);
        offset += 2;
        // param length
        fw_cmd.params_len = BCM43430A1_hcd[offset];
        offset += 1;
        // params
        for (int i = 0; i < fw_cmd.params_len; i++)
            fw_cmd.params[i] = BCM43430A1_hcd[offset + i];
        offset += fw_cmd.params_len;

        bt_send_command(&fw_cmd);
        n_packets_sent++;
    }

    printk("Firmware: sent %d packets\n", n_packets_sent);
    assert(n_packets_sent == 121);

    // most events were consumed by flow control in bt_send_command.
    // drain whatever is left in the queue or pending on the wire.
    delay_ms(100);
    while (pl011_has_data() || !pq_empty(&module.event_rx_buffer)) {
        if (!pq_empty(&module.event_rx_buffer)) {
            pq_pop(&module.event_rx_buffer);
        } else {
            _receive_packet();
        }
    }

    printk("Waiting 250ms for patch to take effect...\n");
    delay_ms(250); // same time as linux
}

// Functions for pulling from the receive buffers

struct hci_event_pkt *bt_receive_event(void) {
    assert(module.initialized);
    while (pq_empty(&module.event_rx_buffer)) {
        _receive_packet();
    }
    return pq_pop(&module.event_rx_buffer);
}

struct hci_event_pkt *bt_receive_event_async(void) {
    assert(module.initialized);
    if (pq_empty(&module.event_rx_buffer)) {
        if (pl011_has_data()) {
            _receive_packet();
            if (pq_empty(&module.event_rx_buffer)) {
                return NULL;
            }
        } else {
            return NULL;
        }
    }
    return pq_pop(&module.event_rx_buffer);
}

struct hci_acl_data_pkt *bt_receive_acl(void) {
    assert(module.initialized);
    while (pq_empty(&module.acl_rx_buffer)) {
        _receive_packet();
    }
    return pq_pop(&module.acl_rx_buffer);
}

struct hci_acl_data_pkt *bt_receive_acl_async(void) {
    assert(module.initialized);
    if (pq_empty(&module.acl_rx_buffer)) {
        if (pl011_has_data()) {
            _receive_packet();
            if (pq_empty(&module.acl_rx_buffer)) {
                return NULL;
            }
        } else {
            return NULL;
        }
    }
    return pq_pop(&module.acl_rx_buffer);
}
