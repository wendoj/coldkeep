// SPDX-License-Identifier: Apache-2.0
//
// View helpers over StoreModel. Nothing here invents a number: everything is
// read from the model, which is built only from recorded or live events.
import type { CaseRun, CaseType, Decision, Mode, StoreModel } from "@coldkeep/shared";
import { computeReadouts } from "@coldkeep/shared";

export interface Series {
  store: Mode;
  points: { t: number; v: number }[];
}

export const CASE_TYPE_LABEL: Record<CaseType, string> = { chilled: "Chilled multideck", frozen: "Frozen well", coldroom: "Walk-in cold room" };

/** Rule names as docs/RULES.md shows them in the UI. */
export const RULE_NAME: Record<string, string> = {
  G1: "Setpoint band",
  G2: "Rate limit",
  G3: "Defrost envelope",
  G4: "Product-temperature override",
  G5: "Needs the store key",
  G6: "Fail-safe hold",
};

/** Both stores' series on one time axis, for a chart with one column per store. */
export function mergeSeries(series: Series[]): { t: number; legacy?: number; protected?: number }[] {
  const rows = new Map<number, { t: number; legacy?: number; protected?: number }>();
  for (const s of series) {
    for (const p of s.points) {
      const row = rows.get(p.t) ?? { t: p.t };
      row[s.store] = p.v;
      rows.set(p.t, row);
    }
  }
  return [...rows.values()].sort((a, b) => a.t - b.t);
}

/** Rules a store's governor used, most frequent first (LEGACY and ALLOW without a rule are left out). */
export function ruleCounts(s: StoreModel): { rule: string; n: number }[] {
  const m = new Map<string, number>();
  for (const d of s.decisions) if (/^G\d$/.test(d.rule)) m.set(d.rule, (m.get(d.rule) ?? 0) + 1);
  return [...m].map(([rule, n]) => ({ rule, n })).sort((a, b) => b.n - a.n || a.rule.localeCompare(b.rule));
}

/** G4 warning thresholds from the default envelope (docs/RULES.md). */
export const WARN_C: Record<CaseType, number> = { chilled: 6, frozen: -19, coldroom: 4 };

export type CaseStatus = "ok" | "warn" | "over";

export function caseStatus(c: CaseRun): CaseStatus {
  const p = c.latest ? c.latest.prod_true ?? c.latest.prod : null;
  if (p === null) return "ok";
  if (p > c.info.limit) return "over";
  if (p > WARN_C[c.info.type]) return "warn";
  return "ok";
}

/** Worst (warmest) product temperature of one case type, per sample, as a chart series. */
export function worstSeries(s: StoreModel, type: CaseType, maxPoints = 600): Series {
  const idx = s.cases.map((c, i) => (c && c.info.type === type ? i : -1)).filter((i) => i >= 0);
  let pts = s.history
    .map((h) => {
      const vs = idx.map((i) => h.prod[i]).filter((v): v is number => typeof v === "number");
      return vs.length ? { t: h.t, v: Math.max(...vs) } : null;
    })
    .filter((p): p is { t: number; v: number } => p !== null);
  if (pts.length > maxPoints) {
    const step = Math.ceil(pts.length / maxPoints);
    pts = pts.filter((_, i) => i % step === 0 || i === pts.length - 1);
  }
  return { store: s.mode, points: pts };
}

export const isApplied = (d: Decision) => d.verdict === "ALLOW" || d.verdict === "CLAMP";

export function verdictText(d: Decision): string {
  if (d.rule === "LEGACY") return d.verdict === "ALLOW" ? "APPLIED" : "FAILED";
  return d.verdict;
}

export interface PairedRow {
  key: string;
  cmd: string;
  caseId: number | undefined;
  value: number | null | undefined;
  rows: Partial<Record<Mode, Decision>>;
}

/** Line up the same command on both stores: the n-th (cmd, case) on one store with the n-th on the other. */
export function pairDecisions(stores: StoreModel[]): PairedRow[] {
  const out: PairedRow[] = [];
  const byKey = new Map<string, PairedRow>();
  for (const s of stores) {
    const seen = new Map<string, number>();
    for (const d of s.decisions) {
      const base = `${d.cmd}#${d.case ?? 0}`;
      const n = (seen.get(base) ?? 0) + 1;
      seen.set(base, n);
      const key = `${base}#${n}`;
      let row = byKey.get(key);
      if (!row) {
        row = { key, cmd: d.cmd, caseId: d.case, value: d.value, rows: {} };
        byKey.set(key, row);
        out.push(row);
      }
      row.rows[s.mode] = d;
    }
  }
  const t = (r: PairedRow) => Math.min(...Object.values(r.rows).map((d) => d!.t));
  return out.sort((a, b) => t(a) - t(b));
}

/** Markdown incident report for one store, from its events only. */
export function incidentReport(s: StoreModel): string {
  const r = computeReadouts(s);
  const m = s.meta;
  const lines = [
    `# Coldkeep incident report: ${s.mode} store`,
    "",
    `- Source: ${m?.source ?? "unknown"}${m?.label ? ` (${m.label})` : ""}${m?.git ? `, commit ${m.git}` : ""}`,
    `- Seed ${m?.seed ?? "?"}, speed ${m?.speed ?? "?"}x, last sample at sim t=${s.t.toFixed(0)} s`,
    `- Readouts (${r.source === "summary" ? "from the recording's summary line" : "computed from samples so far"}):`,
    `  - case-minutes with product above limit: ${r.minutesAbove.toFixed(1)}`,
    `  - peak chilled product temperature: ${r.peakChilled === null ? "n/a" : r.peakChilled.toFixed(1) + " °C"}`,
    `  - commands applied / blocked: ${r.applied} / ${r.blocked}`,
    `  - illustrative food at risk: ${r.foodAtRiskKg} kg`,
    ...(r.a3 ? [`  - privileged actions that succeeded: ${r.a3.succeeded} / ${r.a3.attempts}`] : []),
    "",
    "## Commands",
    "",
    "| sim t (s) | command | case | value | verdict | rule | reason |",
    "|---|---|---|---|---|---|---|",
    ...s.decisions.map(
      (d) => `| ${d.t.toFixed(0)} | ${d.cmd} | ${d.case ?? ""} | ${d.value ?? ""} | ${verdictText(d)} | ${d.rule} | ${d.reason ?? ""} |`,
    ),
    "",
    "## Privileged attempts and events",
    "",
    ...s.log.map((l) => `- t=${l.t.toFixed(0)} [${l.kind}] ${l.text}`),
    "",
    "## Peak product temperature per case",
    "",
    "| case | type | limit °C | peak °C | minutes above |",
    "|---|---|---|---|---|",
    ...s.cases.filter(Boolean).map((c) => {
      const sc = s.summary?.cases.find((x) => x.id === c.info.id);
      const peak = sc?.peak_product ?? c.peakProduct;
      const mins = sc?.minutes_above ?? c.sAbove / 60;
      return `| ${c.info.id} | ${c.info.type} | ${c.info.limit} | ${Number.isFinite(peak) ? peak.toFixed(1) : "n/a"} | ${mins.toFixed(1)} |`;
    }),
    "",
    "Simulated plant; food quantities are illustrative. Not a food-safety certification.",
    "",
  ];
  return lines.join("\n");
}
