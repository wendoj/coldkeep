// SPDX-License-Identifier: Apache-2.0
//
// auditd's ring records, as forwarded by the bridge, are text:
//   t=601 mgmt/admin start_defrost case=1 value=86400.0 -> CLAMP G3 max defrost 45 min
//   t=64 compliance 1:3.3/3.1 2:3.2/3.0 ...
//   t=67 event governord started (fresh state)
// This turns them back into recording events.
import type { EventOf } from "./schemas";
import { Verdict } from "./schemas";

const DECISION =
  /^t=(\d+(?:\.\d+)?) (\S+?)\/(\S*) (\S+) case=(\d+) value=(-?[\d.]+|nan|-?inf) -> (\S+) (\S+) ?(.*)$/i;
const COMPLIANCE = /^t=(\d+(?:\.\d+)?) compliance (.*)$/;
const EVENT = /^t=(\d+(?:\.\d+)?) event (.*)$/;

export type AuditParsed = EventOf<"decision"> | EventOf<"compliance"> | EventOf<"note">;

export function parseAuditText(text: string): AuditParsed | null {
  let m = DECISION.exec(text);
  if (m) {
    const v = Verdict.safeParse(m[7]);
    if (!v.success) return null;
    const value = Number(m[6]);
    return {
      type: "decision",
      t: Number(m[1]),
      origin: m[2],
      user: m[3],
      cmd: m[4]!,
      case: Number(m[5]),
      value: Number.isFinite(value) ? value : null,
      verdict: v.data,
      rule: m[8]!,
      reason: m[9] ?? "",
    };
  }
  m = COMPLIANCE.exec(text);
  if (m) {
    const cases = [];
    for (const part of m[2]!.split(" ")) {
      const c = /^(\d+):(-?[\d.]+)\/(-?[\d.]+)$/.exec(part);
      if (c) cases.push({ id: Number(c[1]), air: Number(c[2]), prod: Number(c[3]) });
    }
    return { type: "compliance", t: Number(m[1]), cases };
  }
  m = EVENT.exec(text);
  if (m) return { type: "note", t: Number(m[1]), text: m[2]! };
  return null;
}
