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
| `metrics.c` | `auditd`, `bridge` | Minutes above limit, peaks, verdict counts, compliance period |
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
