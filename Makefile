PROGS = btnode.c
COMMON_SRC = net.c kbucket.c dht.c bt-multi.c bt.c pl011.c BCM43430A1.c

STAFF_OBJS += $(CS140E_2026_PATH)/libpi/staff-objs/staff-kmalloc.o
STAFF_OBJS += $(CS140E_2026_PATH)/libpi/staff-objs/interrupts-asm.o

BOOTLOADER = my-install
OPT_LEVEL = -Og
RUN = 0

LIBGCC = $(shell arm-none-eabi-gcc -print-libgcc-file-name)
LIB_POST += $(LIBGCC)

include $(CS140E_2026_PATH)/libpi/mk/Makefile.robust-v2
