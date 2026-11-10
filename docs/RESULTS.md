# Results

**Only numbers from this file may appear in the README, UI copy or video**.

## QNX target runs (A1–A4)

Five comparison runs, recorded with `scripts/run-compare.py --label qemu-a1a4-runN` and summarised with `scripts/summarize-runs.py docs/evidence/runs/qemu-a1a4-run` (output: [qemu-a1a4-summary.md](evidence/runs/qemu-a1a4-summary.md)). Both stores run at the same time, each on its own QEMU instance:

- the legacy store on slot 1;
- the protected store on slot 0.

Run settings:

- **Platform:** QNX 8.0 x86_64, emulated (`-cpu max`, 2 vCPUs, no hardware acceleration) on an Apple Silicon Mac.
- **Scenario:** seed 7, speed 30×, 3600 sim-s.
- **Campaigns** (sim times):
  - A1 at t = 600 s;
  - A2 from t = 1150 s (15 s wall quiet, then 15 s wall flood);
  - A3 at t = 2100 s;
  - A4 at t = 2700 s.
- **Commit:** c7b425c. This is the commit the five runs were recorded from; later work
  (including `keyconsole`, commit 97d2178) is not part of these measurements.

Values are mean (min–max) over the 5 runs. The raw files are in [evidence/runs/](evidence/runs/).

### A1 (credentialed takeover) and A3 (compromised web process)

| Metric | Legacy | COLDKEEP |
|---|---|---|
| Case-minutes with product above limit (8 cases) | 75.6 (75.3–76.0) | 0.0 |
| Peak chilled product temperature, °C (limit 8) | 14.0 | 3.2 |
| A1 commands applied (of 17) | 17 | 3 |
| A1 commands blocked (of 17) | 0 | 14 |
| Illustrative food at risk, kg | 2200 | 0 |
| A3 privileged actions that succeeded (of those that apply) | 2/2 | 0/4 |

An earlier set of five A1 + A3 runs (commit 3ed5de1, files `qemu-runN.*`) gave the same verdicts and 76.1 (75.9–76.3) case-minutes for the legacy store.

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

### A2: request flood vs control-loop jitter

How the measurement works:

- Each `casectl` keeps a cumulative histogram of its loop jitter. The runner reads all 8 histograms three times: before a 15 s quiet window, between it and a 15 s flood window, and after the flood.
- The flood comes from `ckflood` on the target. It uses 4 loopback connections, runs as `mgmtd`'s uid at the default priority, and sends pipelined `status` requests. It runs on the target because in trial runs (not recorded) QEMU's user-mode network limited a flood from the host to a few hundred requests/s, which did not load the guest.
- `mgmtd` answered every request it could. It was the bottleneck at about 720 replies/s.

| Metric | Legacy | COLDKEEP |
|---|---|---|
| Flood requests answered by `mgmtd` in 15 s | 10846 (10171–11167) | 10749 (10042–11080) |
| Loop ticks measured, quiet / flood | 37479 / 40363 | 37421 / 40553 |
| Ticks with jitter ≥ 5 ms, %, quiet | 0.55 (0.06–1.08) | 0.73 (0.40–1.36) |
| Ticks with jitter ≥ 5 ms, %, flood | 0.41 (0.06–0.70) | 0.36 (0.13–0.66) |
| Ticks with jitter ≥ 20 ms, %, quiet | 0.00 (0.00–0.01) | 0.00 |
| Ticks with jitter ≥ 20 ms, %, flood | 0.01 (0.00–0.06) | 0.00 (0.00–0.02) |

The flood didn't increase control-loop jitter in either store. `casectl` runs at priority 40 and `mgmtd` at 10 in both, so what this shows is QNX priority scheduling protecting the loop, not a difference between the two architectures.

The share of ticks with jitter ≥ 1 ms (39–87 % in both windows, both stores) is high and varies from run to run. We think the emulated timer explains it, since the controller period is 3.33 ms at 30× and the system tick is 1 ms; treat the absolute jitter here as a property of the emulator rather than of QNX on real hardware.

### A4: governor crash and hang (protected store; the legacy store has no governor)

`sup` (target/sup) runs `casectl` ×8, `governord`, `brokerd` and `mgmtd` (and `keyconsole` in later builds, not in these runs). It restarts any of them that dies, and it measures the time until the restarted process's resource manager is attached. QNX's High Availability Manager is not in the image ([QNX_FEATURES.md](QNX_FEATURES.md)).

| Metric | COLDKEEP |
|---|---|
| `governord` restart after `kill -9`, until its resource manager is ready, ms | 29.6 (28.5–32.7) |
| Renewed 24 h defrost on case 1 right after the restart | DENY, G3 min 4 h between manual defrosts (state restored from disk) in every run |
| Cases that entered G6 hold while `governord` was frozen (SIGSTOP) for 5 s, of 8 | 8 |
| First hold after the freeze, s wall (heartbeat timeout 2 s) | 2.18 (2.16–2.20) |
| Cases released after the governor resumed, of 8 | 8 |

During the freeze the controllers kept their last-known-good configuration and refused to start a defrost (G6). In all five runs no case went above its limit.

## Live check: dashboard Control path and G5 approval (manual, not a measured run)

The five runs above show `disable_alarms` reaching `NEEDS_KEY`, but none includes an approval: they were recorded before `keyconsole`. The dashboard's Control path (browser → relay → `mgmtd` → `brokerd` → `governord`) and the approval at the store console were checked by hand on freshly deployed QEMU stores on 2026-11-10. This is a single functional check, not a measurement, and adds nothing to the tables above. Transcript: [evidence/live-control-check.txt](evidence/live-control-check.txt).

| Request, sent to both stores from Control | Legacy | COLDKEEP |
|---|---|---|
| Setpoint +25 °C on case 1 | ALLOW (applied) | DENY G1 |
| 24 h defrost on case 1 | ALLOW (86400 s) | CLAMP G3 (2700 s) |
| Disable alarms | ALLOW | NEEDS_KEY G5 (key request #1) |

- As `mgmtd`'s uid 1105, writing `approve 1` to `/dev/coldkeep/key` failed with `Permission denied`.
- As root at the store console, `approve 1` was accepted. The audit journal records `keyconsole/admin disable_alarms -> ALLOW G5 alarms disabled with store key`.
- The dashboard's Key requests view showed no approve button.
- Repeating the 24 h defrost on the same case gave `DENY G3` on COLDKEEP (manual defrost too recent) and a legacy controller refusal (`Resource busy`, case already defrosting).

## Host simulator runs (labelled; not QNX measurements)

Produced by `./sim-host/ck-sim --scenario scenarios/demo-main.yaml --mode both` (seed 7) and committed as `tests/golden/demo-main.*.jsonl`. They show the logic working, not QNX enforcing it, and their A3 errnos come from a host model.

| Metric (demo-main, seed 7, 3600 sim-s) | Legacy | COLDKEEP |
|---|---|---|
| Case-minutes with product above limit (8 cases) | 85.3 | 0.0 |
| Peak product temperature, chilled case 1 (limit 8 °C) | 13.9 °C | 3.1 °C |
| Longest defrost cycle, case 1 | 2998 s (forced, still running at end) | 196 s (clamped to 45 min, ended by coil termination) |
| Commands applied / blocked | 17 / 0 | 2 / 15 |
| Illustrative food at risk | 5200 kg | 0 kg |

Regenerate with `make golden` and update this table from the `summary` lines if the model changes.
