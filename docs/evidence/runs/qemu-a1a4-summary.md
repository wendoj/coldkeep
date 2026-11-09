Runs: 5 legacy, 5 protected; seed 7, speed 30x, A1 at t=600 s, A3 at t=2100 s, end t=3600 s (sim time); commit c7b425c. Values are mean (min–max) over runs.

| Metric | Legacy | COLDKEEP |
|---|---|---|
| Case-minutes with product above limit (8 cases) | 75.6 (75.3–76.0) | 0.0 |
| Peak chilled product temperature, °C (limit 8) | 14.0 (14.0–14.0) | 3.2 (3.2–3.2) |
| A1 commands applied (of 17) | 17 | 3 |
| A1 commands blocked (of 17) | 0 | 14 |
| Illustrative food at risk, kg | 2200 | 0 |
| Worst casectl loop jitter seen, µs | 17296 (13284–24931) | 15465 (12795–19622) |
| A3 privileged actions that succeeded (applicable) | 2/2 | 0/4 |

A2: casectl loop jitter, quiet window vs equally long flood window (15 s wall each; all 8 controllers):

| Metric | Legacy | COLDKEEP |
|---|---|---|
| Flood requests answered by mgmtd | 10846 (10171–11167) | 10749 (10042–11080) |
| Loop ticks measured (quiet) | 37479 (37269–37608) | 37421 (37348–37487) |
| Loop ticks measured (flood) | 40363 (39624–40976) | 40553 (40057–41370) |
| Ticks with jitter ≥ 1 ms, % (quiet) | 72.82 (41.24–86.56) | 68.80 (39.35–86.20) |
| Ticks with jitter ≥ 1 ms, % (flood) | 67.93 (42.54–76.52) | 66.91 (44.27–76.38) |
| Ticks with jitter ≥ 5 ms, % (quiet) | 0.55 (0.06–1.08) | 0.73 (0.40–1.36) |
| Ticks with jitter ≥ 5 ms, % (flood) | 0.41 (0.06–0.70) | 0.36 (0.13–0.66) |
| Ticks with jitter ≥ 20 ms, % (quiet) | 0.00 (0.00–0.01) | 0.00 |
| Ticks with jitter ≥ 20 ms, % (flood) | 0.01 (0.00–0.06) | 0.00 (0.00–0.02) |

A4 (protected store only; the legacy store has no governor):

| Metric | COLDKEEP |
|---|---|
| governord restart, kill -9 to resource manager ready, ms | 29.6 (28.5–32.7) |
| Renewed 24 h defrost right after the restart | DENY G3 |
| Cases in G6 hold while governord was frozen 5 s (of 8) | 8 |
| First hold after the freeze, s wall (timeout 2 s) | 2.18 (2.16–2.20) |
| Cases released after resume (of 8) | 8 |

Files: `docs/evidence/runs/qemu-a1a4-run1.legacy.jsonl`, `docs/evidence/runs/qemu-a1a4-run2.legacy.jsonl`, `docs/evidence/runs/qemu-a1a4-run3.legacy.jsonl`, `docs/evidence/runs/qemu-a1a4-run4.legacy.jsonl`, `docs/evidence/runs/qemu-a1a4-run5.legacy.jsonl`, `docs/evidence/runs/qemu-a1a4-run1.protected.jsonl`, `docs/evidence/runs/qemu-a1a4-run2.protected.jsonl`, `docs/evidence/runs/qemu-a1a4-run3.protected.jsonl`, `docs/evidence/runs/qemu-a1a4-run4.protected.jsonl`, `docs/evidence/runs/qemu-a1a4-run5.protected.jsonl`
