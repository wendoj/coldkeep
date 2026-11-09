// SPDX-License-Identifier: Apache-2.0
//
// Schemas for every line COLDKEEP records or streams. One format covers three
// sources: the host simulator (sim-host), comparison runs on QEMU
// (scripts/run-compare.py) and the live target bridge (target/bridge).
// Unknown fields are kept (looseObject), so newer recordings still parse.
import { z } from "zod";

export const CaseType = z.enum(["chilled", "frozen", "coldroom"]);
export type CaseType = z.infer<typeof CaseType>;

export const Mode = z.enum(["legacy", "protected"]);
export type Mode = z.infer<typeof Mode>;

export const Verdict = z.enum(["ALLOW", "CLAMP", "DENY", "NEEDS_KEY"]);
export type Verdict = z.infer<typeof Verdict>;

const num = z.number();
const flag = z.union([z.number(), z.boolean()]).transform((v) => (typeof v === "boolean" ? (v ? 1 : 0) : v));

export const CaseInfo = z.looseObject({ id: z.number().int(), type: CaseType, limit: num });
export type CaseInfo = z.infer<typeof CaseInfo>;

export const Meta = z.looseObject({
  type: z.literal("meta"),
  mode: Mode,
  seed: num.optional(),
  speed: num.optional(),
  source: z.string().optional(),
  label: z.string().optional(),
  scenario: z.string().optional(),
  git: z.string().optional(),
  note: z.string().optional(),
  cases: z.array(CaseInfo).optional(),
});

export const CaseSample = z.looseObject({
  id: z.number().int(),
  type: CaseType.optional(),
  air: num,
  prod: num,
  air_true: num.optional(),
  prod_true: num.optional(),
  sp: num,
  comp: flag,
  heat: flag,
  defrost: flag,
  door: flag,
  hold: flag,
  alive: flag.optional(),
  jitter_us_max: num.optional(),
  jitter_us_avg: num.optional(),
});
export type CaseSample = z.infer<typeof CaseSample>;

export const Telemetry = z.looseObject({ type: z.literal("telemetry"), t: num, cases: z.array(CaseSample) });

export const Decision = z.looseObject({
  type: z.literal("decision"),
  t: num,
  id: num.optional(),
  origin: z.string().optional(),
  user: z.string().optional(),
  cmd: z.string(),
  case: num.optional(),
  value: num.nullable().optional(),
  verdict: Verdict,
  rule: z.string(),
  applied: num.nullable().optional(),
  reason: z.string().optional(),
});
export type Decision = z.infer<typeof Decision>;

export const Attack = z.looseObject({
  type: z.literal("attack"),
  t: num,
  campaign: z.string(),
  action: z.string(),
  result: z.string().optional(),
  source: z.string().optional(),
});

export const Compliance = z.looseObject({
  type: z.literal("compliance"),
  t: num,
  cases: z.array(z.looseObject({ id: z.number().int(), air: num, prod: num })),
});

export const G6 = z.looseObject({ type: z.literal("g6"), t: num, case: num, hold: z.boolean() });
export const Alarm = z.looseObject({ type: z.literal("alarm"), t: num, case: num, product: num, limit: num });
export const Note = z.looseObject({ type: z.literal("note"), t: num, text: z.string() });
export const Proc = z.looseObject({ type: z.literal("proc"), t: num, proc: z.string(), what: z.string() });
/** A raw auditd ring record forwarded by the bridge (see audit.ts for parsing). */
export const Audit = z.looseObject({ type: z.literal("audit"), seq: num, text: z.string() });
export const GovStatus = z.looseObject({ type: z.literal("gov_status"), t: num, text: z.string() });

export const SummaryCase = z.looseObject({
  id: z.number().int(),
  type: CaseType,
  limit: num,
  peak_product: num,
  peak_air: num.optional(),
  minutes_above: num,
});

export const Summary = z.looseObject({
  type: z.literal("summary"),
  mode: Mode,
  minutes_above_total: num,
  food_at_risk_kg: num,
  // sim-host names
  applied: num.optional(),
  blocked: num.optional(),
  // run-compare names
  a1_applied: num.optional(),
  a1_blocked: num.optional(),
  a3_succeeded: num.optional(),
  a3_attempts: num.optional(),
  max_jitter_us: num.optional(),
  cases: z.array(SummaryCase),
});
export type Summary = z.infer<typeof Summary>;

export const Event = z.discriminatedUnion("type", [
  Meta,
  Telemetry,
  Decision,
  Attack,
  Compliance,
  G6,
  Alarm,
  Note,
  Proc,
  Audit,
  GovStatus,
  Summary,
]);
export type Event = z.infer<typeof Event>;
export type EventOf<T extends Event["type"]> = Extract<Event, { type: T }>;

/** Relay -> dashboard WebSocket messages. */
export const RelayMessage = z.discriminatedUnion("kind", [
  z.object({
    kind: z.literal("hello"),
    source: z.enum(["live", "replay"]),
    stores: z.array(Mode),
    files: z.array(z.string()).optional(),
  }),
  z.object({ kind: z.literal("event"), store: Mode, event: Event }),
  z.object({ kind: z.literal("status"), store: Mode, connected: z.boolean(), detail: z.string().optional() }),
  z.object({ kind: z.literal("end"), store: Mode }),
]);
export type RelayMessage = z.infer<typeof RelayMessage>;

/** Parse one JSONL line; returns null (with the zod error) for lines that don't match. */
export function parseLine(line: string): { event: Event } | { error: string } {
  let raw: unknown;
  try {
    raw = JSON.parse(line);
  } catch (e) {
    return { error: `not JSON: ${(e as Error).message}` };
  }
  const r = Event.safeParse(raw);
  return r.success ? { event: r.data } : { error: r.error.issues.map((i) => `${i.path.join(".")}: ${i.message}`).join("; ") };
}
