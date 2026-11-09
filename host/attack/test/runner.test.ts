// SPDX-License-Identifier: Apache-2.0
//
// The attack runner against a mock mgmtd that speaks the same JSON-lines protocol.
import { createServer, type AddressInfo, type Server } from "node:net";
import { afterEach, describe, expect, it } from "vitest";
import { describe as describeReply } from "../src/campaigns";
import { run } from "../src/runner";

interface Mock {
  server: Server;
  port: number;
  requests: { op: string; [k: string]: unknown }[];
}

function mockMgmtd(mode: "legacy" | "protected"): Promise<Mock> {
  const requests: Mock["requests"] = [];
  let t = 590;
  const server = createServer((sock) => {
    sock.on("error", () => {}); // the flood client hangs up with replies in flight
    let authed = false;
    let buf = "";
    sock.on("data", (d) => {
      buf += d.toString();
      let nl: number;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const req = JSON.parse(buf.slice(0, nl));
        buf = buf.slice(nl + 1);
        requests.push(req);
        const reply = (o: object) => sock.write(JSON.stringify(o) + "\n");
        if (req.op === "login") {
          authed = req.user === "admin" && req.pass === "admin";
          reply(authed ? { ok: true, msg: "logged in" } : { ok: false, error: "bad credentials" });
        } else if (!authed) {
          reply({ ok: false, error: "not logged in" });
        } else if (req.op === "status") {
          t += 5;
          reply({ ok: true, mode, t, cases: [{ id: 1, type: "chilled", sp: 3 }, { id: 2, type: "frozen", sp: -22 }] });
        } else if (mode === "legacy") {
          reply({ ok: true, verdict: "ALLOW", rule: "LEGACY", reason: "legacy: applied directly" });
        } else if (req.op === "start_defrost") {
          reply({ ok: true, verdict: "CLAMP", rule: "G3", reason: "max defrost 45 min" });
        } else if (req.op === "set_setpoint") {
          reply({ ok: true, verdict: "DENY", rule: "G1", reason: `setpoint ${req.value} far outside band` });
        } else {
          reply({ ok: true, verdict: "NEEDS_KEY", rule: "G5", reason: "dangerous: queued as key request #1" });
        }
      }
    });
  });
  return new Promise((r) =>
    server.listen(0, "127.0.0.1", () => r({ server, port: (server.address() as AddressInfo).port, requests })),
  );
}

const mocks: Mock[] = [];
afterEach(() => {
  for (const m of mocks.splice(0)) m.server.close();
});

describe("attack runner", () => {
  it("runs A1 against both stores and prints the transcript", async () => {
    const leg = await mockMgmtd("legacy");
    const pro = await mockMgmtd("protected");
    mocks.push(leg, pro);
    const out: string[] = [];
    const { lines } = await run(
      [
        { store: "legacy", host: "127.0.0.1", port: leg.port },
        { store: "protected", host: "127.0.0.1", port: pro.port },
      ],
      { a1At: 600, a2At: null, a2WallMs: 0, pace: 0, pollMs: 1, log: (l) => out.push(l) },
    );
    // 2 cases: 2 defrosts + 2 setpoints + disable_alarms
    expect(lines.map((l) => l.op)).toEqual(["start_defrost", "start_defrost", "set_setpoint", "set_setpoint", "disable_alarms"]);
    expect(lines[2]!.args).toEqual({ case: 1, value: 28 });
    expect(lines[3]!.args).toEqual({ case: 2, value: 3 });
    expect(out).toContain("logged in to legacy (127.0.0.1:" + leg.port + ") as admin/admin");
    const a1 = out.filter((l) => l.startsWith("[A1]"));
    expect(a1[0]).toMatch(/^\[A1\] t=\d+ start_defrost case=1 seconds=86400 -> legacy: APPLIED \| coldkeep: CLAMP \(G3 max defrost 45 min\)$/);
    expect(a1[4]).toMatch(/disable_alarms -> legacy: APPLIED \| coldkeep: NEEDS_KEY \(G5 dangerous: queued as key request #1\)$/);
    // the runner polled the first store's sim time and waited for t=600 before attacking
    const firstCmd = leg.requests.findIndex((r) => r.op === "start_defrost");
    expect(leg.requests.slice(0, firstCmd).filter((r) => r.op === "status").length).toBeGreaterThanOrEqual(2);
  });

  it("floods with status requests (A2) and counts replies", async () => {
    const pro = await mockMgmtd("protected");
    mocks.push(pro);
    const out: string[] = [];
    const { floods } = await run([{ store: "protected", host: "127.0.0.1", port: pro.port }], {
      a1At: null, a2At: 0, a2WallMs: 300, pace: 0, pollMs: 1, log: (l) => out.push(l),
    });
    expect(floods.protected!.requests).toBeGreaterThan(100);
    expect(floods.protected!.replies).toBeGreaterThan(0);
    expect(out.at(-1)).toMatch(/^\[A2\] flood for 0 s wall -> coldkeep: \d+ requests, \d+ answered/);
  });

  it("refuses to run with bad credentials", async () => {
    const pro = await mockMgmtd("protected");
    mocks.push(pro);
    const { MgmtClient } = await import("../src/client");
    const c = new MgmtClient("127.0.0.1", pro.port);
    await c.connect();
    expect((await c.login("admin", "wrong")).ok).toBe(false);
    expect(await c.request({ op: "status" })).toEqual({ ok: false, error: "not logged in" });
    c.close();
  });

  it("describes replies like the transcript in the spec", () => {
    expect(describeReply({ ok: true, verdict: "ALLOW", rule: "LEGACY" })).toBe("APPLIED");
    expect(describeReply({ ok: true, verdict: "DENY", rule: "G1", reason: "far outside band" })).toBe("DENY (G1 far outside band)");
    expect(describeReply({ ok: false, error: "broker unavailable: x" })).toBe("ERROR (broker unavailable: x)");
  });
});
