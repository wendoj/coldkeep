# COLDKEEP

**A QNX 8.0 refrigeration supervisor that a hacker cannot make unsafe, even after fully compromising its web interface.**

> **Disclaimer.** COLDKEEP is a defensive research prototype built for a hackathon. It uses a simulated refrigeration plant, is not certified for food-safety or industrial use, and does not describe or reproduce any real-world exploit. The "attacker" in the demo is a script that already holds valid admin credentials.

In October and November 2026, attackers reached supermarket refrigeration across Europe through internet-exposed management interfaces with default passwords. The core flaw: **whoever controls the management interface controls the cold.**

COLDKEEP is built the other way round. The network-facing management process (`mgmtd`) is assumed compromised. A separate, high-priority **safety governor** is the only process allowed to command the plant, and it enforces a food-safe envelope that no remote command can override. QNX enforces the separation: `mgmtd` runs with every ability denied and locked, only the governor holds the custom ability the case controllers require, and file ownership decides who can even open each service's channel.

*"Even with the admin password, and even with code running inside the web server, an attacker can't make COLDKEEP's food unsafe, because QNX won't let them reach the cold."*

## Results

Same attack, two stores, recorded on **QNX 8.0 running in QEMU**: 5 runs, seed 7, 3600 sim-s. The attacker logs in with `admin/admin` at t = 600 s and sends 17 commands:

- every case: start a 24-hour defrost;
- every case: raise the setpoint by 25 °C;
- disable alarms.

At t = 2100 s the same privileged actions are tried from a shell with `mgmtd`'s uid.

| | Legacy (flat) | COLDKEEP |
|---|---|---|
| Case-minutes with product above limit | 76.1 (75.9–76.3) | **0.0** |
| Peak chilled product temperature (limit 8 °C) | 14.0 °C | **3.2 °C** |
| Attacker commands applied / blocked | 17 / 0 | **3 / 14** (3 defrosts clamped to 45 min) |
| Privileged actions that succeeded | 2 / 2 | **0 / 4** (`EPERM` / `EACCES` from QNX) |
| Illustrative food at risk | 2200 kg | **0 kg** |

Full table, per-rule breakdown, exact QNX error messages and the raw recordings: [docs/RESULTS.md](docs/RESULTS.md). A2 (request flood vs loop jitter) and A4 (governor restart) are not measured yet. The host simulator gives similar results, but those numbers are labelled separately and are not QNX measurements.

## Run it

### Host only (no QNX needed)

Requires a C11 compiler and `make` (macOS or Linux).

```sh
make test            # core unit + property tests, sim-host golden + determinism
make proptest-full   # 10,000 random command streams x 1,000 commands (~30 s)
make demo            # host simulator: legacy vs COLDKEEP, same seed, attacks A1-A4
```

The host simulator's A3 errnos come from a model of the target's access control; on QNX they come from QNX itself (below).

### On QNX 8.0 (QEMU on macOS)

Needs QNX SDP 8.0 installed at `~/qnx800` with the Linux host tools (free non-commercial licence), Docker Desktop and Homebrew `qemu`.

```sh
bash docker/setup.sh                                    # build container for qcc
scripts/qemu-image.sh ~/coldkeep-qemu/store-a store-a   # minimal QNX image (~150 MB)
scripts/qemu-image.sh ~/coldkeep-qemu/store-b store-b
scripts/qemu-run.sh ~/coldkeep-qemu/store-a --slot 0 --headless   # protected store
scripts/qemu-run.sh ~/coldkeep-qemu/store-b --slot 1 --headless   # legacy store
scripts/qnx-build.sh target                             # cross-compile with qcc
scripts/deploy.sh 0 && scripts/deploy.sh 1
scripts/run-compare.py --label myrun                    # A1 + A3 on both, recorded
scripts/summarize-runs.py recordings/myrun
```

### Dashboard

Needs Node 20+.

```sh
cd host && npm ci
# replay a recorded QEMU run (no QNX needed)
npm run relay -- --replay ../docs/evidence/runs/qemu-run1.legacy.jsonl \
                 --replay ../docs/evidence/runs/qemu-run1.protected.jsonl
npm run build && npm run start -w @coldkeep/dashboard   # http://127.0.0.1:3000

# or live from the two QEMU stores, recording until sim t=3600
npm run relay -- --store legacy=127.0.0.1:17071 --store protected=127.0.0.1:17070 --record live1 --end 3600
npm run attack -- --store legacy=127.0.0.1:18081 --store protected=127.0.0.1:18080 --a1-at 600
```

The compare view takes its figures from the run's own `summary` line. While a live run is still going, they are computed from the samples received so far, and the page says which source it is using. `make host-test` runs the schema, relay and attack-runner tests. CI also builds the dashboard and runs a Playwright smoke test in replay mode.

Everything the QNX features rely on was first checked on the target: [docs/QNX_FEATURES.md](docs/QNX_FEATURES.md).

Environment: QNX SDP 8.0, x86_64 image built with `mkqnximage`, QEMU (emulated, `-cpu max`) on macOS, `qcc -Vgcc_ntox86_64`. CI (GitHub Actions, Ubuntu + macOS) runs every host test; it cannot build the QNX target, which needs the licensed SDK.

## How COLDKEEP uses QNX

| QNX feature | Where |
|---|---|
| Ability drop + lock (`procmgr_ability`) | [`target/common/ck_target.c`](target/common/ck_target.c) (`ck_drop_to`), used by [`target/mgmtd/main.c`](target/mgmtd/main.c) |
| Custom ability (`procmgr_ability_create`), kept by one process | [`target/governord/main.c`](target/governord/main.c) |
| Client ability check in a resource manager (`iofunc_client_info_able`) | [`target/casectl/main.c`](target/casectl/main.c) |
| Resource managers + file ownership as channel access control | [`target/governord/main.c`](target/governord/main.c), [`target/brokerd/main.c`](target/brokerd/main.c), [`target/auditd/main.c`](target/auditd/main.c) |
| Message passing (`MsgSend` untrusted → broker → governor) | [`target/mgmtd/main.c`](target/mgmtd/main.c), [`target/brokerd/main.c`](target/brokerd/main.c) |
| Sender identity on each message (`ConnectClientInfo`) | [`target/common/ck_target.c`](target/common/ck_target.c) (`ck_sender_uid`) |
| Real-time priorities (`SCHED_FIFO`) | every `target/*/main.c`; layout in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| Timers delivering pulses (control loops, heartbeats) | [`target/casectl/main.c`](target/casectl/main.c), [`target/governord/main.c`](target/governord/main.c) |
| Shared memory between processes | [`target/plantsim/main.c`](target/plantsim/main.c) |
| Security policy | supported by the SDK; needs a policy file ([QNX_FEATURES.md](docs/QNX_FEATURES.md), question 3) |

## Layout

```
core/        portable C11: thermal, plant, control (casectl + G6), governor (G1-G5),
             broker, journal, metrics, protocol, prng, store (host wiring)
sim-host/    ck-sim: host simulator, legacy and protected in lockstep
target/      QNX processes: plantsim casectl governord brokerd mgmtd auditd bridge
host/        relay (bridge/replay -> websocket, recorder), attack runner, dashboard
scripts/     QEMU image/run, cross-build, deploy, comparison runs
scenarios/   host simulator scenarios        tests/golden/  host golden outputs
docs/        ARCHITECTURE RULES PLANT_MODEL THREAT_MODEL QNX_FEATURES RESULTS
```

## Why the defaults are `admin/admin`

On purpose. The reported attacks used default credentials. COLDKEEP's claim is that it stays safe **even when** the password is known and the web process is compromised.

## News context

- Technology Magazine — [European supermarket freezers targeted by major cyber attack](https://technologymagazine.com/news/european-supermarket-freezers-targeted-by-major-cyber-attack)
- The Grocer — [The cybersecurity blind spot putting UK supermarkets at risk](https://www.thegrocer.co.uk/the-cybersecurity-blind-spot-putting-uk-supermarkets-at-risk/725414.article)
- Retail Gazette — [UK food suppliers hit by cyber attacks targeting fridge and freezer systems](https://www.retailgazette.co.uk/blog/2026/11/uk-food-suppliers-hit-by-cyber-attacks-targeting-fridge-and-freezer-systems/)
- ITV — [UK meat processing firms hit amid fridge and freezer cyber attacks](https://www.itv.com/news/2026-11-03/uk-meat-processing-firms-hit-amid-fridge-and-freezer-cyber-attacks)
- ShopAppy — [explainer (800+ exposed UK controllers)](https://shopappy.com/?p=12775)

## Licence

Apache-2.0. See [LICENSE](LICENSE).
