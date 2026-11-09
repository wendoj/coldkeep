# COLDKEEP QNX processes. Build inside the QNX SDP environment:
#   scripts/qnx-build.sh target            (x86_64)
#   ARCH=aarch64le scripts/qnx-build.sh target
include common.mk

CORE_SRCS := common.c prng.c thermal.c plant.c control.c governor.c broker.c journal.c \
             metrics.c proto.c
PROCS     ?= plantsim casectl governord brokerd mgmtd auditd bridge

OUT       := build/$(ARCH)
CORE_OBJS := $(CORE_SRCS:%.c=$(OUT)/core/%.o)
COMMON_OBJ := $(OUT)/common/ck_target.o
BINS      := $(PROCS:%=$(OUT)/%)

all: $(BINS)

$(OUT)/core/%.o: ../core/%.c ../core/include/coldkeep.h
	@mkdir -p $(dir $@)
	$(QCC) $(CORE_CFLAGS) -c $< -o $@

$(OUT)/common/%.o: common/%.c common/ck_target.h ../core/include/coldkeep.h
	@mkdir -p $(dir $@)
	$(QCC) $(TARGET_CFLAGS) -c $< -o $@

$(OUT)/%: %/main.c $(CORE_OBJS) $(COMMON_OBJ) common/ck_target.h
	@mkdir -p $(dir $@)
	$(QCC) $(TARGET_CFLAGS) $< $(COMMON_OBJ) $(CORE_OBJS) -o $@ $(LDLIBS)

clean:
	rm -rf build

.PHONY: all clean
