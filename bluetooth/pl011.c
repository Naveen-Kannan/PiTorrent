// PL011 UART driver for BCM2835 (Raspberry Pi)
// Connected to onboard BCM43430 Bluetooth chip via GPIO 30-33
// Interrupt-driven RX with circular buffer

#define CQ_N 131072

#include "rpi.h"
#include "pl011.h"
#include "circular.h"
#include "rpi-interrupts.h"
#include "gpio-high.h"

#include <stdbool.h>

#define GPIO_BT_CTS 30
#define GPIO_BT_RTS 31
#define GPIO_BT_TX 32
#define GPIO_BT_RX 33

#define ADDR(x) (0x20201000 + (x))

// 48 MHz UART clock, 115200 baud
// 48,000,000 / (16 * (26 + 3/64)) = 115177 ~ 115200
#define BAUD_INT_VAL 26
#define BAUD_FRAC_VAL 3

#define UART0_DR        ADDR(0x000)
#define UART0_RSRECR    ADDR(0x004)
#define UART0_FR        ADDR(0x018)
#define UART0_ILPR      ADDR(0x020)
#define UART0_IBRD      ADDR(0x024)
#define UART0_FBRD      ADDR(0x028)
#define UART0_LCRH      ADDR(0x02C)
#define UART0_CR        ADDR(0x030)
#define UART0_IFLS      ADDR(0x034)
#define UART0_IMSC      ADDR(0x038)
#define UART0_RIS       ADDR(0x03c)
#define UART0_MIS       ADDR(0x040)
#define UART0_ICR       ADDR(0x044)
#define UART0_DMACR     ADDR(0x048)
#define UART0_ITCR      ADDR(0x080)
#define UART0_ITIP      ADDR(0x084)
#define UART0_ITOP      ADDR(0x088)
#define UART0_TDR       ADDR(0x08c)

// FR register bits
#define FR_RXFE  (1 << 4)   // RX FIFO empty
#define FR_TXFF  (1 << 5)   // TX FIFO full
#define FR_TXFE  (1 << 7)   // TX FIFO empty
#define FR_BUSY  (1 << 3)   // UART busy

// ICR/IMSC bits
#define INT_RX   (1 << 4)
#define INT_RT   (1 << 6)   // RX timeout

static struct {
    cq_t rx_buffer;
    bool initialized;
} module;


// called within ISR -- no dev_barrier needed
static uint8_t _get8(void) {
    return GET32(UART0_DR) & 0xFF;
}

// called within ISR -- no dev_barrier needed
static int _has_data(void) {
    return (GET32(UART0_FR) & FR_RXFE) == 0;
}

void interrupt_vector(void) {
    dev_barrier();
    uint32_t mis = GET32(UART0_MIS);

    bool rx_interrupt = (mis & INT_RX) != 0;
    bool rx_timeout_interrupt = (mis & INT_RT) != 0;

    if (rx_interrupt || rx_timeout_interrupt) {
        while (_has_data()) {
            cq_push(&module.rx_buffer, _get8());
        }
        if (rx_interrupt) {
            PUT32(UART0_ICR, INT_RX);
        }
        if (rx_timeout_interrupt) {
            PUT32(UART0_ICR, INT_RT);
        }
    } else {
        panic("Unexpected UART interrupt: MIS=0x%08x\n", mis);
    }
    dev_barrier();
}

void pl011_init(void) {
    assert(!module.initialized);
    module.initialized = true;

    cq_init(&module.rx_buffer, 1);

    // disable UART first
    PUT32(UART0_CR, 0);
    dev_barrier();

    // GPIO 30-33: alt function 3 for CTS, RTS, TX, RX
    gpio_hi_set_function(GPIO_BT_CTS, GPIO_FUNC_ALT3);
    gpio_hi_set_function(GPIO_BT_RTS, GPIO_FUNC_ALT3);
    gpio_hi_set_function(GPIO_BT_TX,  GPIO_FUNC_ALT3);
    gpio_hi_set_function(GPIO_BT_RX,  GPIO_FUNC_ALT3);

    // disable pull-ups/downs on all 4 pins
    gpio_hi_pud_off(GPIO_BT_CTS);
    gpio_hi_pud_off(GPIO_BT_RTS);
    gpio_hi_pud_off(GPIO_BT_TX);
    gpio_hi_pud_off(GPIO_BT_RX);

    dev_barrier();

    // install interrupt vector table
    interrupt_init();

    // clear all pending interrupts
    PUT32(UART0_ICR, 0x7FF);

    // baud rate: IBRD=26, FBRD=3
    PUT32(UART0_IBRD, BAUD_INT_VAL);
    PUT32(UART0_FBRD, BAUD_FRAC_VAL);

    // enable RX and RX timeout interrupts
    PUT32(UART0_IMSC, INT_RX | INT_RT);

    // IFLS: trigger RX interrupt at 1/8 full (as soon as data arrives)
    PUT32(UART0_IFLS, 0);

    // LCRH: 8-bit word (bits 6:5 = 0b11), FIFOs enabled (bit 4)
    PUT32(UART0_LCRH, (0b11 << 5) | (1 << 4));

    // enable UART: RTS, TX, RX, UART enable
    PUT32(UART0_CR, 0xb01);

    dev_barrier();

    // enable UART interrupt in IRQ controller (IRQ 57 -> Enable_2 bit 25)
    PUT32(IRQ_Enable_2, (1 << 25));

    enable_interrupts();
    dev_barrier();
}

int pl011_can_put8(void) {
    assert(module.initialized);
    dev_barrier();
    int ret = (GET32(UART0_FR) & FR_TXFF) == 0;
    dev_barrier();
    return ret;
}

void pl011_put8(uint8_t c) {
    while (!pl011_can_put8())
        rpi_wait();
    PUT32(UART0_DR, c);
    dev_barrier();
}

int pl011_tx_is_empty(void) {
    assert(module.initialized);
    dev_barrier();
    uint32_t fr = GET32(UART0_FR);
    // not busy AND tx fifo empty
    int ret = ((fr & FR_BUSY) == 0) && ((fr & FR_TXFE) != 0);
    dev_barrier();
    return ret;
}

// provided functions
int pl011_has_data(void) {
    assert(module.initialized);
    return !cq_empty(&module.rx_buffer);
}

uint8_t pl011_get8(void) {
    assert(module.initialized);
    return cq_pop(&module.rx_buffer);
}

void pl011_flush_tx(void) {
    assert(module.initialized);
    while (!pl011_tx_is_empty())
        rpi_wait();
}

int pl011_get8_async(void) {
    assert(module.initialized);
    if (!pl011_has_data())
        return -1;
    return pl011_get8();
}
