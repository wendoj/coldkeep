# COLDKEEP — host-side build and tests (no QNX SDK needed).
.PHONY: all test core-test sim-test proptest-full golden demo clean
all:
	$(MAKE) -C core
	$(MAKE) -C sim-host

test: core-test sim-test

core-test:
	$(MAKE) -C core test

sim-test:
	$(MAKE) -C sim-host test

# Full property sweep: 10k random runs x 1k commands (about 30 s)
proptest-full:
	$(MAKE) -C core proptest CK_PROP_RUNS=10000 CK_PROP_CMDS=1000

golden:
	$(MAKE) -C sim-host golden

demo: all
	./sim-host/ck-sim --scenario scenarios/demo-main.yaml --mode both --out out --check

clean:
	$(MAKE) -C core clean
	$(MAKE) -C sim-host clean
	rm -rf out
