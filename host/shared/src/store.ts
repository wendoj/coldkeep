// SPDX-License-Identifier: Apache-2.0
//
// One store's state, built only from the events in a recording or live feed.
// The dashboard renders this; the compare readouts come from computeReadouts().
import { parseAuditText } from "./audit";
import type { CaseInfo, CaseSample, CaseType, Decision, Event, EventOf, Mode, Summary } from "./schemas";

/** Food-safe limits and illustrative stock per case type (the same values as core/thermal.c). */
export const LIMIT_C: Record<CaseType, number> = { chilled: 8, frozen: -18, coldroom: 5 };
export const FOOD_KG: Record<CaseType, number> = { chilled: 400, frozen: 300, coldroom: 1500 };

export interface CaseRun {
  info: CaseInfo;
  latest: CaseSample | null;
  sAbove: number; // sim seconds with product above limit, from samples so far
  peakProduct: number;
}

export interface HistoryPoint {
  t: number;
  prod: (number | null)[]; // by case index (case id - 1)
}

export interface LogEntry {
  t: number;
  kind: "attack" | "g6" | "alarm" | "note" | "proc";
  text: string;
}

const HISTORY_MAX = 20000;

export class StoreModel {
  readonly mode: Mode;
  meta: EventOf<"meta"> | null = null;
  cases: CaseRun[] = [];
  t = 0;
  history: HistoryPoint[] = [];
  decisions: Decision[] = [];
  attacks: EventOf<"attack">[] = [];
  compliance: EventOf<"compliance">[] = [];
  log: LogEntry[] = [];
  govStatus: string | null = null;
  summary: Summary | null = null;
  private lastSampleT: number | null = null;

  constructor(mode: Mode) {
    this.mode = mode;
  }

  apply(ev: Event): void {
    switch (ev.type) {
      case "meta":
        this.meta = ev;
        for (const c of ev.cases ?? []) this.caseRun(c.id, c.type).info.limit = c.limit;
        break;
      case "telemetry":
        this.onTelemetry(ev);
        break;
      case "decision":
        this.decisions.push(ev);
        break;
      case "audit": {
        const p = parseAuditText(ev.text);
        if (p) this.apply(p);
        break;
      }
      case "attack":
        this.attacks.push(ev);
        this.log.push({ t: ev.t, kind: "attack", text: `${ev.campaign} ${ev.action}: ${ev.result ?? ""}`.trim() });
        break;
      case "compliance":
        this.compliance.push(ev);
        break;
      case "g6":
        this.log.push({ t: ev.t, kind: "g6", text: `case ${ev.case} ${ev.hold ? "holds last-known-good (G6)" : "released"}` });
        break;
      case "alarm":
        this.log.push({ t: ev.t, kind: "alarm", text: `case ${ev.case} product ${ev.product.toFixed(1)} °C at limit ${ev.limit}` });
        break;
      case "note":
        this.log.push({ t: ev.t, kind: "note", text: ev.text });
        break;
      case "proc":
        this.log.push({ t: ev.t, kind: "proc", text: `${ev.proc} ${ev.what}` });
        break;
      case "gov_status":
        this.govStatus = ev.text;
        break;
      case "summary":
        this.summary = ev;
        break;
    }
  }

  private caseRun(id: number, type?: CaseType): CaseRun {
    let c = this.cases[id - 1];
    if (!c) {
      const ty = type ?? "chilled";
      c = { info: { id, type: ty, limit: LIMIT_C[ty] }, latest: null, sAbove: 0, peakProduct: -Infinity };
      this.cases[id - 1] = c;
    } else if (type && c.info.type !== type && c.latest === null) {
      c.info = { id, type, limit: LIMIT_C[type] };
    }
    return c;
  }

  private onTelemetry(ev: EventOf<"telemetry">) {
    const dt = this.lastSampleT === null ? 0 : ev.t - this.lastSampleT;
    const point: HistoryPoint = { t: ev.t, prod: [] };
    for (const s of ev.cases) {
      const c = this.caseRun(s.id, s.type ?? this.meta?.cases?.find((m) => m.id === s.id)?.type);
      const prod = s.prod_true ?? s.prod;
      c.latest = s;
      c.peakProduct = Math.max(c.peakProduct, prod);
      if (dt > 0 && prod > c.info.limit) c.sAbove += dt;
      point.prod[s.id - 1] = prod;
    }
    this.lastSampleT = ev.t;
    this.t = ev.t;
    this.history.push(point);
    if (this.history.length > HISTORY_MAX) this.history.splice(0, this.history.length - HISTORY_MAX);
  }

  /** Pending key requests: G5 NEEDS_KEY decisions (approval happens at the store, never here). */
  get keyRequests(): Decision[] {
    return this.decisions.filter((d) => d.verdict === "NEEDS_KEY");
  }
}

export interface Readouts {
  /** "summary": the recording's own summary line; "running": computed from the samples received so far. */
  source: "summary" | "running";
  minutesAbove: number;
  peakChilled: number | null;
  applied: number;
  blocked: number;
  foodAtRiskKg: number;
  a3: { succeeded: number; attempts: number } | null;
}

/** Compare-view readouts. Never hard-coded: always derived from the store's events. */
export function computeReadouts(s: StoreModel): Readouts {
  const countVerdicts = () => ({
    applied: s.decisions.filter((d) => d.verdict === "ALLOW" || d.verdict === "CLAMP").length,
    blocked: s.decisions.filter((d) => d.verdict === "DENY" || d.verdict === "NEEDS_KEY").length,
  });
  const a3Attacks = s.attacks.filter((a) => a.campaign === "A3" && a.result && !a.result.startsWith("n/a"));
  if (s.summary) {
    const sm = s.summary;
    const chilled = sm.cases.filter((c) => c.type === "chilled").map((c) => c.peak_product);
    const counts = countVerdicts();
    return {
      source: "summary",
      minutesAbove: sm.minutes_above_total,
      peakChilled: chilled.length ? Math.max(...chilled) : null,
      applied: sm.a1_applied ?? sm.applied ?? counts.applied,
      blocked: sm.a1_blocked ?? sm.blocked ?? counts.blocked,
      foodAtRiskKg: sm.food_at_risk_kg,
      a3:
        sm.a3_attempts !== undefined && sm.a3_succeeded !== undefined
          ? { succeeded: sm.a3_succeeded, attempts: sm.a3_attempts }
          : null,
    };
  }
  const cases = s.cases.filter(Boolean);
  const chilled = cases.filter((c) => c.info.type === "chilled" && c.latest).map((c) => c.peakProduct);
  return {
    source: "running",
    minutesAbove: cases.reduce((a, c) => a + c.sAbove, 0) / 60,
    peakChilled: chilled.length ? Math.max(...chilled) : null,
    ...countVerdicts(),
    foodAtRiskKg: cases.filter((c) => c.sAbove > 0).reduce((a, c) => a + FOOD_KG[c.info.type], 0),
    a3: a3Attacks.length
      ? { succeeded: a3Attacks.filter((a) => a.result === "SUCCEEDED").length, attempts: a3Attacks.length }
      : null,
  };
}

/** Compliance records as CSV (one row per case per record). */
export function complianceCsv(s: StoreModel): string {
  const rows = ["store,t_sim_s,case,type,air_c,product_c,limit_c,above_limit"];
  for (const r of s.compliance) {
    for (const c of r.cases) {
      const info = s.cases[c.id - 1]?.info;
      const limit = info?.limit ?? "";
      rows.push(
        [s.mode, r.t.toFixed(1), c.id, info?.type ?? "", c.air.toFixed(1), c.prod.toFixed(1), limit,
          info && c.prod > info.limit ? "yes" : "no"].join(","),
      );
    }
  }
  return rows.join("\n") + "\n";
}
