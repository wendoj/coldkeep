# Results

**Only numbers from this file may appear in the README, UI copy or video**.

## QNX target runs

Five comparison runs, recorded with `scripts/run-compare.py --label qemu-runN` and summarised with `scripts/summarize-runs.py recordings/qemu-run`. Both stores run at the same time, each on its own QEMU instance:

- the legacy store on slot 1;
- the protected store on slot 0.

Run settings:

- **Platform:** QNX 8.0 x86_64, emulated (`-cpu max`, no hardware acceleration) on an Apple Silicon Mac.
- **Scenario:** seed 7, speed 30×, 3600 sim-s.
- **A1:** at t = 600 s.
- **A3:** at t = 2100 s, as shell commands run with `mgmtd`'s uid.
- **Commit:** 3ed5de1.

Values are mean (min–max) over the 5 runs. The raw files are in [evidence/runs/](evidence/runs/).

| Metric | Legacy | COLDKEEP |
|---|---|---|
| Case-minutes with product above limit (8 cases) | 76.1 (75.9–76.3) | 0.0 |
| Peak chilled product temperature, °C (limit 8) | 14.0 | 3.2 |
| A1 commands applied (of 17) | 17 | 3 |
| A1 commands blocked (of 17) | 0 | 14 |
| Illustrative food at risk, kg | 2200 | 0 |
| A3 privileged actions that succeeded (of those that apply) | 2/2 | 0/4 |

**What COLDKEEP did with the 17 A1 commands (the same in every run):**

- 3 defrosts were clamped to 45 min (G3).
- 5 defrosts were denied, because at most 2 cases may defrost at once (G3).
- 8 setpoints were denied, because they were far outside the band (G1).
- `disable_alarms` was queued as a key request (G5).

**A3 errors on the protected store.** QNX itself returned these, in every run, to commands running with uid 1105 (`mgmtd`):

| Attempt | Error |
|---|---|
| write to case controller | `dd: /dev/coldkeep/case/1: write error: Operation not permitted` (custom ability not held) |
| connect to the governor channel | `cat: /dev/coldkeep/gov: Permission denied` |
| open the audit journal for writing | `sh: cannot create /data/coldkeep/audit.bin: Permission denied` |
| read the governor state | `cat: /data/coldkeep/gov/state: Permission denied` |

On the legacy store, `mgmtd` runs as root:

- The controller write and the journal write both succeeded.
- The governor channel and the governor state do not exist in that architecture, so those two attempts are `n/a`.

**Not measured yet:**

- **A2, loop jitter under a request flood.** The runs recorded the worst `casectl` loop jitter, but only without a flood: 13.9–89.3 ms on the legacy store and 13.2–143.3 ms on the protected store. Under full emulation these values reflect the emulator, not QNX scheduling, so they are not a real-time result.
- **A4, governor restart.** TBD (pending QEMU run).

## Host simulator runs (labelled; not QNX measurements)

Produced by `./sim-host/ck-sim --scenario scenarios/demo-main.yaml --mode both` (seed 7) and committed as `tests/golden/demo-main.*.jsonl`. They show the logic, not QNX's enforcement. The A3 errnos there come from a host model.

| Metric (demo-main, seed 7, 3600 sim-s) | Legacy | COLDKEEP |
|---|---|---|
| Case-minutes with product above limit (8 cases) | 85.3 | 0.0 |
| Peak product temperature, chilled case 1 (limit 8 °C) | 13.9 °C | 3.1 °C |
| Longest defrost cycle, case 1 | 2998 s (forced, still running at end) | 196 s (clamped to 45 min, ended by coil termination) |
| Commands applied / blocked | 17 / 0 | 2 / 15 |
| Illustrative food at risk | 5200 kg | 0 kg |

Regenerate with `make golden` and update this table from the `summary` lines if the model changes.
