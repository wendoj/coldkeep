# COLDKEEP

**A QNX 8.0 refrigeration supervisor that stays food-safe even after someone fully compromises its web interface.**

> **Disclaimer.** COLDKEEP is a defensive research prototype built for a hackathon. The refrigeration plant is simulated, nothing here is certified for food-safety or industrial use, and it doesn't describe or reproduce any real-world exploit; the "attacker" in the demo is a script that already holds valid admin credentials.

In October and November 2026, attackers got into supermarket refrigeration across Europe through management interfaces left on the internet with default passwords (sources at the end). The underlying problem is that **whoever controls the management interface controls the cold.**

COLDKEEP turns that around by assuming the network-facing management process, `mgmtd`, is already compromised. A separate high-priority **safety governor** is the only process allowed to command the plant, and no remote command can push it outside its food-safe envelope. QNX holds the two apart: `mgmtd` runs with every ability denied and locked, the case controllers only accept writes from the holder of a custom ability that only the governor keeps, and file ownership decides who can open each service's channel in the first place.

*"An attacker with the admin password, and even with code running inside the web server, still can't make COLDKEEP's food unsafe, because QNX won't let them near the cold."*

## Results

Same attacks, two stores, recorded on **QNX 8.0 running in QEMU**: 5 runs, seed 7, 3600 sim-s.

- **A1:** the attacker logs in with `admin/admin` and sends 17 commands: a 24-hour defrost on every case, every setpoint raised by 25 °C, and alarms off.
- **A2:** `mgmtd` is flooded from the target.
- **A3:** the attacker tries privileged actions from a shell with `mgmtd`'s uid.
- **A4:** the governor is killed, then frozen.

| | Legacy (flat) | COLDKEEP |
|---|---|---|
| Case-minutes with product above limit | 75.6 (75.3–76.0) | **0.0** |
| Peak chilled product temperature (limit 8 °C) | 14.0 °C | **3.2 °C** |
| Attacker commands applied / blocked | 17 / 0 | **3 / 14** (3 defrosts clamped to 45 min) |
| Privileged actions that succeeded | 2 / 2 | **0 / 4** (`EPERM` / `EACCES` from QNX) |
| Illustrative food at risk | 2200 kg | **0 kg** |
| Loop ticks with jitter ≥ 5 ms, quiet → flood | 0.55 % → 0.41 % | 0.73 % → **0.36 %** |
| Governor restart after `kill -9` | n/a | **29.6 ms** (28.5–32.7) |
| Cases in G6 safe hold while the governor was frozen 5 s | n/a | **8 of 8**, about 2.2 s after the freeze |

Both stores run at the same QNX priorities, so the flood row says something about QNX rather than about the two architectures, and under emulation the absolute jitter mostly reflects QEMU. [docs/RESULTS.md](docs/RESULTS.md) has the full tables, the exact QNX error messages and the raw recordings. The host simulator lands close to these numbers, but it isn't QNX, and RESULTS keeps its figures in a separate, labelled section.

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
npm run build && npm run start -w @coldkeep/dashboard   # http://127.0.0.1:3000

# host simulator: press "Run attack" in the dashboard to pick campaigns and a seed
npm run relay

# or replay a recorded QEMU run (no QNX needed), with pause, speed and restart
npm run relay -- --replay ../docs/evidence/runs/qemu-run1.legacy.jsonl \
                 --replay ../docs/evidence/runs/qemu-run1.protected.jsonl

# or live from the two QEMU stores, recording until sim t=3600;
# --mgmt also enables the dashboard's "Run attack" (A1 and A2) and its Control view
npm run relay -- --store legacy=127.0.0.1:17071 --store protected=127.0.0.1:17070 \
                 --mgmt legacy=127.0.0.1:18081 --mgmt protected=127.0.0.1:18080 --record live1 --end 3600
npm run attack -- --store legacy=127.0.0.1:18081 --store protected=127.0.0.1:18080 --a1-at 600   # same, from a terminal
```

The overview reads its figures from the run's own `summary` line, or computes them from the samples so far while a run is still going, and the header always tells you whether you're looking at the host simulator, a QEMU recording or the live targets. On QNX, A3 and A4 run from the store's own shell, so the dashboard only offers them in the host simulator.

The **Control** view sends single operator commands (`set_setpoint`, `start_defrost`, `stop_defrost`, `set_fans`, `disable_alarms`) to the live stores and shows each store's verdict. It switches off in replay and simulator mode so a recording can't pass for a live command, and nothing in the dashboard can approve a key request. The System view gets relay health through the dashboard's `/api/relay` route while events keep flowing over the relay WebSocket; [docs/DASHBOARD.md](docs/DASHBOARD.md) has the details. `make host-test` runs the schema, relay and attack-runner tests, and CI also builds the dashboard and runs the Playwright tests in replay and simulator mode.

Before relying on any QNX feature we checked it on the target first; [docs/QNX_FEATURES.md](docs/QNX_FEATURES.md) has what we found.

Environment: QNX SDP 8.0, an x86_64 image built with `mkqnximage`, QEMU (emulated, `-cpu max`) on macOS, and `qcc -Vgcc_ntox86_64`. CI on GitHub Actions runs every host test on Ubuntu and macOS, but it can't build the QNX target because that needs the licensed SDK.

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
| Second custom ability (`coldkeep.envelope.write`) checked on each message (`ConnectClientInfoAble`) | [`target/keyconsole/main.c`](target/keyconsole/main.c), [`target/governord/main.c`](target/governord/main.c) |
| Restart and fail-safe (`posix_spawn`/`waitpid` supervisor; HAM is not in the image) | [`target/sup/main.c`](target/sup/main.c), G6 in [`core/control.c`](core/control.c) |
| Security policy | supported by the SDK; needs a policy file ([QNX_FEATURES.md](docs/QNX_FEATURES.md), question 3). Not used: the fallback is abilities + uid + file permissions |

## Layout

```
core/        portable C11: thermal, plant, control (casectl + G6), governor (G1-G5),
             broker, journal, metrics, protocol, prng, store (host wiring)
sim-host/    ck-sim: host simulator, legacy and protected in lockstep
target/      QNX processes: plantsim casectl governord brokerd mgmtd auditd bridge
             keyconsole sup, and ckflood (A2 load generator)
host/        relay (bridge/replay -> websocket, recorder), attack runner, dashboard
scripts/     QEMU image/run, cross-build, deploy, comparison runs
scenarios/   host simulator scenarios        tests/golden/  host golden outputs
docs/        ARCHITECTURE RULES PLANT_MODEL THREAT_MODEL QNX_FEATURES RESULTS
             DASHBOARD CODELAB
```

## Why the defaults are `admin/admin`

That's deliberate. The reported attacks got in with default credentials, and COLDKEEP's whole claim is that it stays safe **even when** the attacker knows the password and controls the web process.

## News context

- Technology Magazine — [European supermarket freezers targeted by major cyber attack](https://technologymagazine.com/news/european-supermarket-freezers-targeted-by-major-cyber-attack)
- The Grocer — [The cybersecurity blind spot putting UK supermarkets at risk](https://www.thegrocer.co.uk/the-cybersecurity-blind-spot-putting-uk-supermarkets-at-risk/725414.article)
- Retail Gazette — [UK food suppliers hit by cyber attacks targeting fridge and freezer systems](https://www.retailgazette.co.uk/blog/2026/11/uk-food-suppliers-hit-by-cyber-attacks-targeting-fridge-and-freezer-systems/)
- ITV — [UK meat processing firms hit amid fridge and freezer cyber attacks](https://www.itv.com/news/2026-11-03/uk-meat-processing-firms-hit-amid-fridge-and-freezer-cyber-attacks)
- ShopAppy — [explainer (800+ exposed UK controllers)](https://shopappy.com/?p=12775)

## Licence

Apache-2.0. See [LICENSE](LICENSE).
