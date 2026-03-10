# Build all test programs at once
PROGS = 1-test-pl011.c 2-hci-reset.c 3-firmware-upload.c 4-read-bdaddr.c

COMMON_SRC += bt.c pl011.c BCM43430A1.c

# staff objects for kmalloc and interrupt vector table
STAFF_OBJS += $(CS140E_2026_PATH)/libpi/staff-objs/staff-kmalloc.o
STAFF_OBJS += $(CS140E_2026_PATH)/libpi/staff-objs/interrupts-asm.o

BOOTLOADER = my-install
OPT_LEVEL = -Og
RUN = 0

LIBGCC = $(shell arm-none-eabi-gcc -print-libgcc-file-name)
LIB_POST += $(LIBGCC)

include $(CS140E_2026_PATH)/libpi/mk/Makefile.robust-v2
