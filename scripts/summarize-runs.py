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
    print("\nFiles: " + ", ".join(f"`{p}`" for p, _, _ in leg + pro))
    return 0


if __name__ == "__main__":
    sys.exit(main())
