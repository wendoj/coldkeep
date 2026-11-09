// SPDX-License-Identifier: Apache-2.0
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { Event, RelayMessage, StoreModel, computeReadouts, complianceCsv, parseAuditText, parseLine } from "../src";

const ROOT = join(import.meta.dirname, "../../..");
const recordings = [
  ...readdirSync(join(ROOT, "tests/golden")).map((f) => join(ROOT, "tests/golden", f)),
  ...readdirSync(join(ROOT, "docs/evidence/runs"))
    .filter((f) => f.endsWith(".jsonl"))
    .map((f) => join(ROOT, "docs/evidence/runs", f)),
];

function load(path: string, keep: (e: Event) => boolean = () => true): StoreModel {
  const lines = readFileSync(path, "utf8").trim().split("\n");
  const first = parseLine(lines[0]!);
  if (!("event" in first) || first.event.type !== "meta") throw new Error(`${path}: no meta`);
  const s = new StoreModel(first.event.mode);
  for (const l of lines) {
    const r = parseLine(l);
    if ("event" in r && keep(r.event)) s.apply(r.event);
  }
  return s;
}

describe("schemas", () => {
  it.each(recordings)("every line of %s parses", (path) => {
    const lines = readFileSync(path, "utf8").trim().split("\n");
    for (const [i, l] of lines.entries()) {
      const r = parseLine(l);
      expect("error" in r ? `${path}:${i + 1}: ${r.error}` : "ok").toBe("ok");
    }
  });

  it("rejects malformed events", () => {
    expect("error" in parseLine("{")).toBe(true);
    expect("error" in parseLine('{"type":"decision","t":1,"cmd":"x","verdict":"MAYBE","rule":"G1"}')).toBe(true);
    expect("error" in parseLine('{"type":"telemetry","t":"soon","cases":[]}')).toBe(true);
    expect("error" in parseLine('{"type":"warp_drive","t":1}')).toBe(true);
  });

  it("bridge lines parse", () => {
    const meta =
      '{"type":"meta","source":"qnx-bridge","mode":"protected","seed":7,"speed":30.0,"cases":[{"id":1,"type":"chilled","limit":8.0}]}';
    const tel =
      '{"type":"telemetry","t":120.5,"cases":[{"id":1,"type":"chilled","air":3.48,"prod":3.03,"air_true":3.518,"prod_true":3.022,"sp":3.0,"comp":0,"heat":0,"defrost":0,"door":1,"hold":0,"alive":1,"jitter_us_max":1186,"jitter_us_avg":40}]}';
    for (const l of [meta, tel, '{"type":"audit","seq":3,"text":"t=67 event governord started"}', '{"type":"gov_status","t":1,"text":"envelope v1\\n"}'])
      expect("event" in parseLine(l)).toBe(true);
  });

  it("relay messages validate", () => {
    expect(RelayMessage.safeParse({ kind: "hello", source: "replay", stores: ["legacy", "protected"] }).success).toBe(true);
    expect(RelayMessage.safeParse({ kind: "event", store: "legacy", event: { type: "note", t: 1, text: "x" } }).success).toBe(true);
    expect(RelayMessage.safeParse({ kind: "event", store: "both", event: { type: "note", t: 1, text: "x" } }).success).toBe(false);
  });
});

describe("audit text", () => {
  it("parses a governor decision", () => {
    expect(parseAuditText("t=601 mgmt/admin start_defrost case=1 value=86400.0 -> CLAMP G3 max defrost 45 min")).toEqual({
      type: "decision", t: 601, origin: "mgmt", user: "admin", cmd: "start_defrost", case: 1, value: 86400,
      verdict: "CLAMP", rule: "G3", reason: "max defrost 45 min",
    });
  });
  it("parses a legacy decision and a NaN value", () => {
    const d = parseAuditText("t=620 mgmt/admin disable_alarms case=0 value=nan -> ALLOW LEGACY legacy: applied directly");
    expect(d).toMatchObject({ verdict: "ALLOW", rule: "LEGACY", value: null, reason: "legacy: applied directly" });
  });
  it("parses compliance and events", () => {
    expect(parseAuditText("t=64 compliance 1:3.3/3.1 5:-21.7/-22.0")).toEqual({
      type: "compliance", t: 64, cases: [{ id: 1, air: 3.3, prod: 3.1 }, { id: 5, air: -21.7, prod: -22 }],
    });
    expect(parseAuditText("t=67 event governord started (fresh state)")).toEqual({ type: "note", t: 67, text: "governord started (fresh state)" });
    expect(parseAuditText("garbage")).toBeNull();
  });
});

describe("compare readouts are computed from data", () => {
  it("use the recording's summary line when there is one", () => {
    for (const mode of ["legacy", "protected"] as const) {
      const path = join(ROOT, `docs/evidence/runs/qemu-run1.${mode}.jsonl`);
      const s = load(path);
      const raw = readFileSync(path, "utf8").trim().split("\n").map((l) => JSON.parse(l));
      const sm = raw.find((o) => o.type === "summary");
      const r = computeReadouts(s);
      expect(r.source).toBe("summary");
      expect(r.minutesAbove).toBe(sm.minutes_above_total);
      expect(r.applied).toBe(sm.a1_applied);
      expect(r.blocked).toBe(sm.a1_blocked);
      expect(r.foodAtRiskKg).toBe(sm.food_at_risk_kg);
      expect(r.a3).toEqual({ succeeded: sm.a3_succeeded, attempts: sm.a3_attempts });
    }
  });

  it("change when the data changes (nothing is hard-coded)", () => {
    const s = new StoreModel("legacy");
    s.apply({ type: "meta", mode: "legacy", cases: [{ id: 1, type: "chilled", limit: 8 }, { id: 2, type: "coldroom", limit: 5 }] });
    const sample = (id: number, prod: number) => ({ id, air: prod, prod, sp: 3, comp: 0, heat: 0, defrost: 0, door: 0, hold: 0 });
    s.apply({ type: "telemetry", t: 0, cases: [sample(1, 3), sample(2, 2)] });
    s.apply({ type: "telemetry", t: 60, cases: [sample(1, 9), sample(2, 2)] });
    s.apply({ type: "telemetry", t: 180, cases: [sample(1, 10.5), sample(2, 2)] });
    s.apply({ type: "decision", t: 61, cmd: "set_setpoint", verdict: "ALLOW", rule: "LEGACY" });
    s.apply({ type: "decision", t: 62, cmd: "set_setpoint", verdict: "DENY", rule: "G1" });
    s.apply({ type: "decision", t: 63, cmd: "disable_alarms", verdict: "NEEDS_KEY", rule: "G5" });
    const r = computeReadouts(s);
    expect(r).toMatchObject({ source: "running", minutesAbove: 3, peakChilled: 10.5, applied: 1, blocked: 2, foodAtRiskKg: 400, a3: null });
    s.apply({ type: "telemetry", t: 240, cases: [sample(1, 4), sample(2, 6)] });
    expect(computeReadouts(s)).toMatchObject({ minutesAbove: 4, foodAtRiskKg: 1900 });
  });

  it("running readouts from telemetry agree with the run's summary", () => {
    for (const mode of ["legacy", "protected"] as const) {
      const path = join(ROOT, `docs/evidence/runs/qemu-run1.${mode}.jsonl`);
      const full = computeReadouts(load(path));
      // telemetry is recorded every 30 sim-s, the summary used 0.25 s polling: allow one sample per case
      const running = computeReadouts(load(path, (e) => e.type !== "summary"));
      expect(running.source).toBe("running");
      expect(Math.abs(running.minutesAbove - full.minutesAbove)).toBeLessThan(8 * 0.5 + 1);
      expect(running.applied).toBe(full.applied);
      expect(running.blocked).toBe(full.blocked);
      expect(running.foodAtRiskKg).toBe(full.foodAtRiskKg);
    }
  });

  it("legacy fails and COLDKEEP holds in the host golden run", () => {
    const leg = computeReadouts(load(join(ROOT, "tests/golden/demo-main.legacy.jsonl")));
    const pro = computeReadouts(load(join(ROOT, "tests/golden/demo-main.protected.jsonl")));
    expect(leg.minutesAbove).toBeGreaterThan(0);
    expect(pro.minutesAbove).toBe(0);
    expect(pro.blocked).toBeGreaterThan(leg.blocked);
  });
});

describe("compliance csv", () => {
  it("has one row per case per record", () => {
    const s = load(join(ROOT, "tests/golden/demo-main.legacy.jsonl"));
    const rows = complianceCsv(s).trim().split("\n");
    expect(rows[0]).toBe("store,t_sim_s,case,type,air_c,product_c,limit_c,above_limit");
    expect(rows.length).toBe(1 + s.compliance.length * 8);
    expect(rows.some((r) => r.endsWith(",yes"))).toBe(true);
  });
});
