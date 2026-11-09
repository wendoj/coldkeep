# QNX feature checks. Build inside the QNX SDP environment (qnxsdp-env.sh sourced).
#   make               x86_64 target (QEMU quick start image)
#   make ARCH=aarch64le
ARCH    ?= x86_64
QCC     := qcc -Vgcc_nto$(ARCH)
CFLAGS  := -Wall -O2

all: able_server able_client

able_%: able_%.c
	$(QCC) $(CFLAGS) -o $@ $<

clean:
	rm -f able_server able_client

.PHONY: all clean
