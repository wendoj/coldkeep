# Results

**Only numbers from this file may appear in the README, UI copy or video**.

## QNX target runs

5 runs per mode on QEMU. Not done yet.

| Metric | Legacy | COLDKEEP |
|---|---|---|
| A1: minutes above limit (all cases) | TBD (pending QEMU run) | TBD (pending QEMU run) |
| A1: peak product temperature | TBD (pending QEMU run) | TBD (pending QEMU run) |
| A2: `casectl` loop jitter, with vs without flood | TBD (pending QEMU run) | TBD (pending QEMU run) |
| A3: privileged attempts blocked | TBD (pending QEMU run) | TBD (pending QEMU run) |
| A4: governor restart time; unsafe actions during restart | n/a | TBD (pending QEMU run) |

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
