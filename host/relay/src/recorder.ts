// SPDX-License-Identifier: Apache-2.0
//
// Records a live session as recordings/<label>.<mode>.jsonl, in the same format as
// scripts/run-compare.py, and closes each file with a summary computed from the
// recorded samples (bridge telemetry, about one sample per wall-clock second).
import { execFileSync } from "node:child_process";
import { createWriteStream, mkdirSync, type WriteStream } from "node:fs";
import { join } from "node:path";
import { StoreModel, computeReadouts, type Event, type Mode } from "@coldkeep/shared";

export function gitSha(cwd: string): string {
  try {
    return execFileSync("git", ["-C", cwd, "rev-parse", "--short", "HEAD"], { encoding: "utf8" }).trim();
  } catch {
    return "unknown";
  }
}

export class Recorder {
  private files = new Map<Mode, { out: WriteStream; model: StoreModel; metaWritten: boolean }>();

  constructor(
    private dir: string,
    private label: string,
    private git: string,
  ) {
    mkdirSync(dir, { recursive: true });
  }

  path(store: Mode) {
    return join(this.dir, `${this.label}.${store}.jsonl`);
  }

  record(store: Mode, ev: Event) {
    let f = this.files.get(store);
    if (!f) {
      f = { out: createWriteStream(this.path(store)), model: new StoreModel(store), metaWritten: false };
      this.files.set(store, f);
    }
    if (ev.type === "meta") {
      if (f.metaWritten) return; // a reconnect resends meta; keep the first
      f.metaWritten = true;
      ev = { ...ev, label: this.label, git: this.git, date: new Date().toISOString(), recorder: "host relay" };
    } else if (!f.metaWritten) {
      return; // everything after a meta line, so the file is self-describing
    }
    f.model.apply(ev);
    f.out.write(JSON.stringify(ev) + "\n");
  }

  /** Write a summary line per store and close the files. */
  async close(): Promise<string[]> {
    const paths: string[] = [];
    for (const [store, f] of this.files) {
      const m = f.model;
      const r = computeReadouts(m);
      const cases = m.cases
        .filter((c) => c && Number.isFinite(c.peakProduct))
        .map((c) => ({
          id: c.info.id,
          type: c.info.type,
          limit: c.info.limit,
          peak_product: Math.round(c.peakProduct * 1000) / 1000,
          minutes_above: Math.round((c.sAbove / 60) * 100) / 100,
        }));
      const summary = {
        type: "summary",
        mode: store,
        source: "host relay (bridge telemetry)",
        t_end: m.t,
        samples: m.history.length,
        minutes_above_total: Math.round(r.minutesAbove * 100) / 100,
        food_at_risk_kg: r.foodAtRiskKg,
        a1_applied: r.applied,
        a1_blocked: r.blocked,
        cases,
      };
      await new Promise<void>((res) => f.out.end(JSON.stringify(summary) + "\n", () => res()));
      paths.push(this.path(store));
    }
    return paths;
  }
}
