# COLDKEEP host simulator. `make` builds ./ck-sim; `make test` runs golden,
# determinism and qualitative checks; `make golden` regenerates tests/golden/.
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror \
           -Wno-unused-parameter -ffp-contract=off -I../core/include
CORE    := ../core/build/libcoldkeep.a
ROOT    := ..
SCEN    := demo-main a1 a2 a3 a4
GOLDEN  := $(ROOT)/tests/golden
TMP     := build/out

.PHONY: all test golden check determinism clean core
all: ck-sim

core:
	$(MAKE) -C ../core

$(CORE): core

ck-sim: main.c scenario.c scenario.h ../core/include/coldkeep.h core
	$(CC) $(CFLAGS) main.c scenario.c $(CORE) -lm -o $@

# Every scenario: protected never exceeds limits; legacy exceeds where expected.
check: ck-sim
	@for s in $(SCEN); do \
	  ./ck-sim --scenario $(ROOT)/scenarios/$$s.yaml --mode both --out $(TMP) --quiet --check || exit 1; \
	done

# Same seed -> byte-identical output.
determinism: ck-sim
	@mkdir -p build/d1 build/d2
	@./ck-sim --scenario $(ROOT)/scenarios/demo-main.yaml --out build/d1 --quiet
	@./ck-sim --scenario $(ROOT)/scenarios/demo-main.yaml --out build/d2 --quiet
	@cmp build/d1/demo-main.legacy.jsonl build/d2/demo-main.legacy.jsonl
	@cmp build/d1/demo-main.protected.jsonl build/d2/demo-main.protected.jsonl
	@./ck-sim --scenario $(ROOT)/scenarios/demo-main.yaml --seed 8 --out build/d2 --quiet
	@! cmp -s build/d1/demo-main.protected.jsonl build/d2/demo-main.protected.jsonl
	@echo "determinism: OK (same seed identical, different seed differs)"

# Golden outputs: committed JSONL for every scenario, compared byte for byte.
test: check determinism
	@mkdir -p $(TMP)
	@for s in $(SCEN); do \
	  ./ck-sim --scenario $(ROOT)/scenarios/$$s.yaml --mode both --out $(TMP) --quiet; \
	  for m in legacy protected; do \
	    cmp $(TMP)/$$s.$$m.jsonl $(GOLDEN)/$$s.$$m.jsonl || { echo "golden mismatch: $$s.$$m (run 'make golden' if intended)"; exit 1; }; \
	  done; \
	done
	@echo "golden: OK ($(words $(SCEN)) scenarios x 2 modes)"

golden: ck-sim
	@mkdir -p $(GOLDEN)
	@for s in $(SCEN); do ./ck-sim --scenario $(ROOT)/scenarios/$$s.yaml --mode both --out $(GOLDEN) --quiet; done
	@echo "regenerated $(GOLDEN)"

clean:
	rm -rf build ck-sim
