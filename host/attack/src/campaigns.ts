// SPDX-License-Identifier: Apache-2.0
//
// Attack campaigns. They use only mgmtd's documented API and the default
// credentials; nothing else. A3 (privileged actions from inside the web process)
// and A4 (killing the governor) are run on the target itself, not from here: see
// scripts/run-compare.py and docs/DEMO_SCRIPT.md.
import type { MgmtClient, Reply, Status } from "./client";

export interface Step {
  campaign: string;
  op: string;
  args: Record<string, number>;
}

/** A1, credentialed takeover: 24 h defrost on every case, setpoints +25 °C, alarms off. */
export function a1Steps(st: Status): Step[] {
  return [
    ...st.cases.map((c) => ({ campaign: "A1", op: "start_defrost", args: { case: c.id, seconds: 86400 } })),
    ...st.cases.map((c) => ({ campaign: "A1", op: "set_setpoint", args: { case: c.id, value: c.sp + 25 } })),
    { campaign: "A1", op: "disable_alarms", args: {} },
  ];
}

export function describe(r: Reply): string {
  if (!r.ok && r.error) return `ERROR (${r.error})`;
  if (r.rule === "LEGACY") return r.verdict === "ALLOW" ? "APPLIED" : "FAILED";
  if (!r.verdict) return r.ok ? "OK" : "FAILED";
  return r.rule && r.rule !== "-" ? `${r.verdict} (${r.rule} ${r.reason ?? ""})`.replace(" )", ")") : r.verdict;
}

export interface FloodResult {
  requests: number;
  replies: number;
  wallMs: number;
}

/**
 * A2, API flood: send status requests as fast as the socket accepts them for
 * `wallMs`, keeping at most `window` requests in flight.
 */
export async function flood(c: MgmtClient, wallMs: number, window = 512): Promise<FloodResult> {
  const t0 = Date.now();
  let sent = 0;
  while (Date.now() - t0 < wallMs) {
    while (c.pending < window) {
      c.sendNoWait({ op: "status" });
      sent++;
    }
    await new Promise((r) => setImmediate(r));
  }
  const replies = sent - c.pending;
  return { requests: sent, replies, wallMs: Date.now() - t0 };
}
