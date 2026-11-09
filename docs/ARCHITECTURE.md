# Architecture

How the code is split between the portable core, the QNX processes and the host simulator.

## Portable core → QNX processes

All safety logic lives in [`core/`](../core) as plain C11, with no QNX headers. Each QNX process in `target/` is a thin wrapper around one module:

| Core module | Target process | What it does |
|---|---|---|
| `thermal.c` | `plantsim` | Lumped thermal model per case ([PLANT_MODEL.md](PLANT_MODEL.md)) |
| `control.c` | `casectl` ×N | Thermostat, defrost sequencing, G6 fail-safe hold |
| `governor.c` | `governord` | G1–G5, defrost schedule, last-known-good, status text |
| `broker.c` | `brokerd`, `keyconsole` | Schema, rate limit, classification, key-request queue |
| `journal.c` | `auditd` | Append-only CRC32 journal with torn-tail recovery |
| `metrics.c` | `auditd` | Minutes above limit, peaks, verdict counts, compliance period |
| `prng.c` | all | Seeded splitmix64; same seed → same run |
| `store.c` | *(host only)* | In-process wiring of one store for `sim-host` and property tests |

## Host simulator

[`sim-host/ck-sim`](../sim-host) runs a scenario on a legacy store and a protected store in lockstep: same seed, same shopper load, same attack actions at the same sim times. It writes one JSONL file per store and prints a combined transcript.

```
[A1] t=601 start_defrost case=1 seconds=86400 -> legacy: APPLIED | coldkeep: CLAMP (G3 max defrost 45 min)
```

JSONL event types: `meta`, `telemetry` (every 30 sim-s), `decision`, `attack`, `alarm`, `g6`, `proc`, `note`, `compliance` (every 15 sim-min) and `summary`. A relay's replay mode can consume the same format.

## What the host model does *not* show

- **A2 control-loop jitter.** The host is one process, so it only shows the broker's per-session rate limit. Jitter must be measured on QNX.
- **A3 errnos.** These come from a table in `ck_store_compromised_attempt()` that records what the target's access control *must* return. The output labels them `"source":"host-model"`. On the target the errnos come from QNX itself.
- **Governor restart.** The host keeps governor state (rate-limit history, last manual defrost) across a restart. On the target, `governord` must restore this from the journal, or an attacker could crash it to reset G2/G3.

## On QNX

Each module runs as its own process, under its own uid, at its own `SCHED_FIFO` priority
([`target/`](../target), started by [`target/scripts/coldkeep-start.sh`](../target/scripts/coldkeep-start.sh)).

| Process | uid | Priority | Owns | Who may use it |
|---|---|---|---|---|
| `plantsim` | 1101 | 30 | shared memory `/coldkeep_plant` (probe readings, sim clock) | everyone reads; only `plantsim` writes |
| `casectl` ×N | 1102 | 40 | `/dev/coldkeep/case/N`; its slot in `/coldkeep_act` | anyone may read; `devctl`/write needs the custom ability `coldkeep.plant.write` (protected mode) |
| `governord` | 1103 | 35 | `/dev/coldkeep/gov` (owner 1104, mode 0600), `/dev/coldkeep/status` | only `brokerd` can open the command channel; sender uid checked again on each message |
| `brokerd` | 1104 | 25 | `/dev/coldkeep/broker` (owner 1105, mode 0600) | only `mgmtd` |
| `auditd` | 1106 | 20 | `/dev/coldkeep/audit`; `/data/coldkeep/audit.bin` (0600) | records accepted from trusted uids only |
| `mgmtd` | 1105 | 10 | TCP 8080 (JSON lines) | the network; holds **no** abilities (all denied and locked) |
| `bridge` | 1107 | 8 | TCP 7070 (JSON lines, read-only) | the host relay; holds **no** abilities, opens no controller, maps shared memory read-only |

`governord` creates `coldkeep.plant.write` and is the only process that keeps it when it
drops root; on QNX 8.0 even root does not hold a newly created custom ability
([QNX_FEATURES.md](QNX_FEATURES.md), question 1). Messages between processes are sent
with `MsgSend` on a file descriptor opened on the server's path, so the file mode and
owner decide who can connect at all.

The `bridge` runs in both modes. Once per wall-clock second it sends a `telemetry` line,
plus every new record from `auditd`'s ring. `auditd` numbers each ring record (`#<seq>`),
so the bridge never sends a record twice. In protected mode the bridge also sends the
governor's status text every 5 s. Clients cannot send it commands.

**Legacy mode** runs the same plant, controllers and auditor, but `casectl -L` performs
no ability check and `mgmtd -L` stays root and writes the controllers directly; there is
no governor or broker.

**Fail-safe (G6).** `governord` sends a heartbeat `devctl` to every controller every
250 ms of wall time; a controller that misses heartbeats for 2 s of wall time holds its
last-known-good configuration and refuses to start a defrost.

**Comparison runs.** [`scripts/run-compare.py`](../scripts/run-compare.py) restarts a
legacy store (QEMU slot 1) and a protected store (slot 0) with the same seed and speed,
runs A1 through the API and A3 as shell commands with `mgmtd`'s uid, and records both to
`recordings/`. Plant timing on QEMU follows the wall clock, so two runs with the same seed
are similar but not byte-identical (unlike `sim-host`).

## Host side

[`host/`](../host) is an npm workspace (Node 20+, TypeScript):

| Package | What it does |
|---|---|
| [`shared`](../host/shared) | zod schemas for every recorded or streamed line, plus `StoreModel`, which builds one store's state from events. `computeReadouts()` takes the compare figures from the run's `summary` line, or computes them from the samples while a run is still going. |
| [`relay`](../host/relay) | **Live:** connects to each store's bridge (QEMU forwards 7070 to 17070 + slot) and turns audit records into `decision`, `compliance` and `note` events. **Replay:** plays two recordings on one sim clock. Either way it serves WebSocket on port 7071. With `--record` it writes `recordings/<label>.<mode>.jsonl` in the `run-compare.py` format, and `--end` stops at a fixed sim time. |
| [`attack`](../host/attack) | Runs A1 (credentialed takeover) and A2 (status flood) against both stores' `mgmtd` (8080 forwarded to 18080 + slot) at scripted sim times, and prints the transcript. A3 and A4 run on the target ([`scripts/run-compare.py`](../scripts/run-compare.py)). |
| [`dashboard`](../host/dashboard) | Next.js app with five views: store, compare, attack console, audit & compliance (with CSV export and an incident report), and key requests. It has no approve button: key requests are approved at the store's local console. |

```
QNX (QEMU slot 1, legacy)    bridge :7070 ─┐                   ┌─ dashboard :3000
QNX (QEMU slot 0, protected) bridge :7070 ─┼─► relay :7071 ws ─┤
recordings/*.jsonl (replay) ───────────────┘                   └─ recordings/<label>.*.jsonl
attack runner ──► mgmtd :8080 on each store
```
