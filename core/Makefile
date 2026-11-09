# COLDKEEP core — portable C11. `make test` builds and runs unit + property tests.
CC      ?= cc
# -ffp-contract=off: no fused multiply-add, so float results match on every platform.
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror \
           -Wno-unused-parameter -ffp-contract=off -Iinclude
BUILD   := build
SRCS    := common.c prng.c thermal.c control.c governor.c broker.c journal.c metrics.c store.c
OBJS    := $(SRCS:%.c=$(BUILD)/%.o)
LIB     := $(BUILD)/libcoldkeep.a

UNIT_SRCS := tests/test_main.c tests/test_prng.c tests/test_thermal.c tests/test_control.c \
             tests/test_governor.c tests/test_broker.c tests/test_journal.c tests/test_metrics.c \
             tests/test_store.c

CK_PROP_RUNS ?= 1000
CK_PROP_CMDS ?= 1000

.PHONY: all test unit proptest clean
all: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c include/coldkeep.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	rm -f $@
	ar rcs $@ $^

$(BUILD)/unit_tests: $(UNIT_SRCS) tests/ck_test.h $(LIB)
	$(CC) $(CFLAGS) -Itests $(UNIT_SRCS) $(LIB) -lm -o $@

$(BUILD)/prop_tests: tests/test_property.c tests/ck_test.h $(LIB)
	$(CC) $(CFLAGS) -Itests tests/test_property.c $(LIB) -lm -o $@

unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

proptest: $(BUILD)/prop_tests
	CK_PROP_RUNS=$(CK_PROP_RUNS) CK_PROP_CMDS=$(CK_PROP_CMDS) ./$(BUILD)/prop_tests

test: unit proptest

clean:
	rm -rf $(BUILD)
