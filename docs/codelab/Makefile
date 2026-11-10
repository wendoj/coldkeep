# Build with the QNX SDP environment sourced:  source ~/qnx800/qnxsdp-env.sh
#   make                  x86_64 (QEMU, most PCs)
#   make ARCH=aarch64le   Raspberry Pi 4
ARCH   ?= x86_64
CC     := qcc -Vgcc_nto$(ARCH)
CFLAGS := -Wall -O2

all: valved valvectl

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f valved valvectl

.PHONY: all clean
