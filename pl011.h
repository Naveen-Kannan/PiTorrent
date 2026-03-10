#ifndef PL011_H
#define PL011_H

#include <stdint.h>

void pl011_init(void);
uint8_t pl011_get8(void);
void pl011_put8(uint8_t c);
int pl011_get8_async(void);
int pl011_has_data(void);
int pl011_can_put8(void);
void pl011_flush_tx(void);

#endif
