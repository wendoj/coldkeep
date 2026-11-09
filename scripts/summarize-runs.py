#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarise recorded comparison runs as a Markdown table, computed only from the
`summary` lines that scripts/run-compare.py wrote.

  scripts/summarize-runs.py recordings/qemu-run      # matches qemu-run*.{legacy,protected}.jsonl
"""
import glob, json, statistics, sys


def load(prefix, mode):
    runs = []
    for path in sorted(glob.glob(f"{prefix}*.{mode}.jsonl")):
        meta, summary = None, None
        with open(path) as f:
            for line in f:
                o = json.loads(line)
                if o["type"] == "meta":
                    meta = o
                elif o["type"] == "summary":
                    summary = o
        if meta and summary:
            runs.append((path, meta, summary))
    return runs


def rng(vals, fmt="{:.1f}"):
    if not vals:
        return "—"
    lo, hi, mean = min(vals), max(vals), statistics.mean(vals)
    if lo == hi:
        return fmt.format(lo)
    return f"{fmt.format(mean)} ({fmt.format(lo)}–{fmt.format(hi)})"


def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else "recordings/qemu-run"
    leg, pro = load(prefix, "legacy"), load(prefix, "protected")
    if not leg or not pro:
        print(f"no complete runs for {prefix}", file=sys.stderr)
        return 1
    m = leg[0][1]

    def col(runs, fn, fmt="{:.1f}"):
        return rng([fn(s) for _, _, s in runs], fmt)

    def peak(s):
        return max(c["peak_product"] for c in s["cases"] if c["type"] == "chilled")

    def a3(s):
        return f'{s["a3_succeeded"]}/{s["a3_attempts"]}'

    rows = [
        ("Case-minutes with product above limit (8 cases)", lambda s: s["minutes_above_total"], "{:.1f}"),
        ("Peak chilled product temperature, °C (limit 8)", peak, "{:.1f}"),
        ("A1 commands applied (of 17)", lambda s: s["a1_applied"], "{:.0f}"),
        ("A1 commands blocked (of 17)", lambda s: s["a1_blocked"], "{:.0f}"),
        ("Illustrative food at risk, kg", lambda s: s["food_at_risk_kg"], "{:.0f}"),
        ("Worst casectl loop jitter seen, µs", lambda s: s["max_jitter_us"], "{:.0f}"),
    ]
    print(f"Runs: {len(leg)} legacy, {len(pro)} protected; seed {m['seed']}, speed {m['speed']:g}x, "
          f"A1 at t={m['attack_at']:g} s, A3 at t={m['a3_at']:g} s, end t={m['end']:g} s (sim time); "
          f"commit {m['git']}. Values are mean (min–max) over runs.\n")
    print("| Metric | Legacy | COLDKEEP |")
    print("|---|---|---|")
    for name, fn, fmt in rows:
        print(f"| {name} | {col(leg, fn, fmt)} | {col(pro, fn, fmt)} |")
    a3l = sorted({a3(s) for _, _, s in leg})
    a3p = sorted({a3(s) for _, _, s in pro})
    print(f"| A3 privileged actions that succeeded (applicable) | {', '.join(a3l)} | {', '.join(a3p)} |")

    def a2(key, fld):
        return lambda s: (s.get("a2") or {}).get(key, {}).get(fld) if key else (s.get("a2") or {}).get(fld)

    def pct(fn):
        return lambda s: None if fn(s) is None else 100.0 * fn(s)

    def col2(runs, fn, fmt):
        vals = [fn(s) for _, _, s in runs if fn(s) is not None]
        return rng(vals, fmt)

    if any(s.get("a2") for _, _, s in leg + pro):
        print("\nA2: casectl loop jitter, quiet window vs equally long flood window "
              f"({leg[0][2]['a2']['window_s']:g} s wall each; all 8 controllers):\n")
        print("| Metric | Legacy | COLDKEEP |")
        print("|---|---|---|")
        print(f"| Flood requests answered by mgmtd | {col2(leg, a2(None, 'replies'), '{:.0f}')} | {col2(pro, a2(None, 'replies'), '{:.0f}')} |")
        for w in ("quiet", "flood"):
            print(f"| Loop ticks measured ({w}) | {col2(leg, a2(w, 'ticks'), '{:.0f}')} | {col2(pro, a2(w, 'ticks'), '{:.0f}')} |")
        for fld, name in (("share_ge_1ms", "≥ 1 ms"), ("share_ge_5ms", "≥ 5 ms"), ("share_ge_20ms", "≥ 20 ms")):
            for w in ("quiet", "flood"):
                print(f"| Ticks with jitter {name}, % ({w}) | {col2(leg, pct(a2(w, fld)), '{:.2f}')} | "
                      f"{col2(pro, pct(a2(w, fld)), '{:.2f}')} |")

    a4 = [s["a4"] for _, _, s in pro if s.get("a4")]
    if a4:
        print("\nA4 (protected store only; the legacy store has no governor):\n")
        print("| Metric | COLDKEEP |")
        print("|---|---|")
        print(f"| governord restart, kill -9 to resource manager ready, ms | {rng([x['restart_ms'] for x in a4 if x['restart_ms'] is not None])} |")
        print(f"| Renewed 24 h defrost right after the restart | {', '.join(sorted({str(x['after_restart_verdict']) + ' ' + str(x.get('after_restart_rule', '')) for x in a4}))} |")
        print(f"| Cases in G6 hold while governord was frozen 5 s (of 8) | {rng([x['hold_cases'] for x in a4], '{:.0f}')} |")
        print(f"| First hold after the freeze, s wall (timeout 2 s) | {rng([x['hold_after_s'] for x in a4 if x['hold_after_s'] is not None], '{:.2f}')} |")
        print(f"| Cases released after resume (of 8) | {rng([x['released_cases'] for x in a4], '{:.0f}')} |")
    print("\nFiles: " + ", ".join(f"`{p}`" for p, _, _ in leg + pro))
    return 0


if __name__ == "__main__":
    sys.exit(main())
