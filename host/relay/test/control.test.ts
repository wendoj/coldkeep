// SPDX-License-Identifier: Apache-2.0
//
// Dashboard requests over the WebSocket: playback controls, host-simulator runs
// and live campaigns against a mock mgmtd.
import { createServer, type AddressInfo } from "node:net";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import WebSocket from "ws";
import { RelayMessage, type Capabilities, type ClientMessage } from "@coldkeep/shared";
import { Controller } from "../src/control";
import { Hub } from "../src/hub";
import { Operator, validateCommand } from "../src/operator";
import { scenarioYaml, simulate } from "../src/sim";
import { loadRecording } from "../src/sources";

const ROOT = join(import.meta.dirname, "../../..");
const RUN = (m: string) => join(ROOT, `docs/evidence/runs/qemu-run1.${m}.jsonl`);

const until = async (cond: () => boolean, ms = 5000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error("timeout");
    await new Promise((r) => setTimeout(r, 10));
  }
};

const cleanup: (() => void)[] = [];
afterEach(() => {
  for (const f of cleanup.splice(0)) f();
});

async function client(hub: Hub) {
  const port = await hub.listen(0);
  const msgs: RelayMessage[] = [];
  const ws = new WebSocket(`ws://127.0.0.1:${port}`);
  ws.on("message", (d) => msgs.push(RelayMessage.parse(JSON.parse(d.toString()))));
  await new Promise((r) => ws.on("open", r));
  cleanup.push(() => {
    ws.close();
    hub.close();
  });
  return { msgs, send: (m: ClientMessage | object) => ws.send(JSON.stringify(m)) };
}

const last = <K extends RelayMessage["kind"]>(msgs: RelayMessage[], kind: K) =>
  msgs.filter((m): m is Extract<RelayMessage, { kind: K }> => m.kind === kind).at(-1);

describe("scenario template", () => {
  it("orders campaigns by sim time and ignores duplicates", () => {
    const y = scenarioYaml(["A3", "A1", "A1"], 42);
    expect(y).toContain("seed: 42");
    expect(y.indexOf("campaign: A1")).toBeLessThan(y.indexOf("campaign: A3"));
    expect(y.match(/campaign: A1/g)).toHaveLength(1);
    expect(y).not.toContain("campaign: A2");
  });
});

describe("host simulator runs", () => {
  it("are deterministic and only contain the chosen campaigns", async () => {
    const a = await simulate(ROOT, ["A1"], 7);
    const b = await simulate(ROOT, ["A1"], 7);
    expect(a.files.map((f) => f.store)).toEqual(["legacy", "protected"]);
    expect(JSON.stringify(a.files[1]!.events)).toBe(JSON.stringify(b.files[1]!.events));
    expect(a.files[0]!.events[0]).toMatchObject({ type: "meta", source: "sim-host" });
    expect(a.transcript.length).toBeGreaterThan(0);
    expect(a.transcript.every((l) => l.text.startsWith("[A1]"))).toBe(true);
  }, 30000);
});

describe("controller", () => {
  it("pauses, resumes, re-rates and restarts a replay", async () => {
    const can = { playback: true, sim: false, live: [] };
    const hub = new Hub({ kind: "hello", source: "replay", stores: [], can });
    const ctl = new Controller(hub, { root: ROOT, can, rate: 60 });
    const { msgs, send } = await client(hub);
    ctl.play([loadRecording(RUN("legacy")), loadRecording(RUN("protected"))], "replay");
    await until(() => last(msgs, "playback")?.paused === false);

    send({ kind: "playback", action: "pause" });
    await until(() => last(msgs, "playback")?.paused === true);
    const frozen = msgs.length;
    await new Promise((r) => setTimeout(r, 200));
    expect(msgs.length).toBe(frozen);

    send({ kind: "rate", rate: "max" });
    send({ kind: "playback", action: "resume" });
    await until(() => last(msgs, "playback")?.done === true);
    expect(msgs.filter((m) => m.kind === "end")).toHaveLength(2);

    send({ kind: "playback", action: "restart" });
    await until(() => msgs.filter((m) => m.kind === "hello").length === 2);
    send({ kind: "nonsense" }); // ignored
  });

  it("runs the simulator on request and streams its transcript", async () => {
    const can = { playback: true, sim: true, live: [] };
    const hub = new Hub({ kind: "hello", source: "sim", stores: [], can });
    new Controller(hub, { root: ROOT, can, rate: 60 });
    const { msgs, send } = await client(hub);
    send({ kind: "run", target: "sim", campaigns: ["A1"], seed: 3, rate: "max" });
    await until(() => last(msgs, "run")?.run.state === "done", 30000);
    await until(() => last(msgs, "playback")?.done === true);
    const hello = last(msgs, "hello")!;
    expect(hello).toMatchObject({ source: "sim", stores: ["legacy", "protected"] });
    expect(msgs.some((m) => m.kind === "log" && m.text.startsWith("[A1]"))).toBe(true);
    expect(msgs.some((m) => m.kind === "event" && m.event.type === "decision")).toBe(true);
  }, 30000);

  it("refuses runs it was not given the capability for", async () => {
    const can = { playback: false, sim: false, live: [] };
    const hub = new Hub({ kind: "hello", source: "live", stores: [], can });
    new Controller(hub, { root: ROOT, can, rate: 1 });
    const { msgs, send } = await client(hub);
    send({ kind: "run", target: "sim", campaigns: ["A1"] });
    send({ kind: "run", target: "live", campaigns: ["A1"] });
    send({ kind: "playback", action: "pause" });
    await new Promise((r) => setTimeout(r, 300));
    expect(msgs.map((m) => m.kind)).toEqual(["hello"]);
  });

  it("sends A1 to live targets through their management API", async () => {
    const ops: string[] = [];
    const server = createServer((sock) => {
      let buf = "";
      sock.on("error", () => {});
      sock.on("data", (d) => {
        buf += d.toString();
        let nl: number;
        while ((nl = buf.indexOf("\n")) >= 0) {
          const req = JSON.parse(buf.slice(0, nl));
          buf = buf.slice(nl + 1);
          ops.push(req.op);
          const reply =
            req.op === "status" ? { ok: true, mode: "protected", t: 700, cases: [{ id: 1, type: "chilled", sp: 3 }] }
            : req.op === "login" ? { ok: true }
            : { ok: true, verdict: "DENY", rule: "G1", reason: "far outside band" };
          sock.write(JSON.stringify(reply) + "\n");
        }
      });
    });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    cleanup.push(() => server.close());
    const port = (server.address() as AddressInfo).port;

    const can = { playback: false, sim: false, live: ["A1" as const] };
    const hub = new Hub({ kind: "hello", source: "live", stores: ["protected"], can });
    new Controller(hub, { root: ROOT, can, rate: 1, mgmt: [{ store: "protected", host: "127.0.0.1", port }] });
    const { msgs, send } = await client(hub);
    send({ kind: "run", target: "live", campaigns: ["A1", "A3"] }); // A3 is not allowed live: dropped
    await until(() => last(msgs, "run")?.run.state === "done", 15000);
    expect(last(msgs, "run")!.run.campaigns).toEqual(["A1"]);
    expect(ops).toContain("set_setpoint");
    expect(msgs.some((m) => m.kind === "log" && m.text.includes("coldkeep: DENY"))).toBe(true);
  }, 20000);
});

/** A mock mgmtd: login always succeeds, then `reply` decides the answer. */
async function mgmtd(reply: (req: Record<string, unknown>) => object) {
  const reqs: Record<string, unknown>[] = [];
  const server = createServer((sock) => {
    let buf = "";
    sock.on("error", () => {});
    sock.on("data", (d) => {
      buf += d.toString();
      let nl: number;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const req = JSON.parse(buf.slice(0, nl)) as Record<string, unknown>;
        buf = buf.slice(nl + 1);
        reqs.push(req);
        sock.write(JSON.stringify(req.op === "login" ? { ok: true } : reply(req)) + "\n");
      }
    });
  });
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
  cleanup.push(() => server.close());
  return { reqs, port: (server.address() as AddressInfo).port };
}

describe("operator commands", () => {
  it("sends one command to mgmtd and broadcasts the decision with its id", async () => {
    const m = await mgmtd((req) =>
      req.op === "set_setpoint"
        ? { ok: true, verdict: "DENY", rule: "G1", case: 1, applied: null, reason: "far outside band" }
        : { ok: true, verdict: "CLONE", rule: "G3", applied: 3 },
    );
    const can: Capabilities = { playback: false, sim: false, live: [], control: ["protected"] };
    const hub = new Hub({ kind: "hello", source: "live", stores: ["protected"], can });
    const op = new Operator([{ store: "protected", host: "127.0.0.1", port: m.port }]);
    cleanup.push(() => op.close());
    new Controller(hub, { root: ROOT, can, rate: 1, operator: op });
    const { msgs, send } = await client(hub);

    send({ kind: "command", id: "c1", store: "protected", op: "set_setpoint", args: { case: 1, value: 28 } });
    await until(() => msgs.some((x) => x.kind === "command_result"));
    const r = last(msgs, "command_result")!;
    expect(r).toMatchObject({
      id: "c1", store: "protected", op: "set_setpoint", args: { case: 1, value: 28 },
      reply: { ok: true, verdict: "DENY", rule: "G1" },
    });
    expect(r.at).toBeGreaterThan(1_600_000_000_000);
    expect(m.reqs.map((q) => q.op)).toEqual(["login", "set_setpoint"]);
    expect(m.reqs[1]).toMatchObject({ op: "set_setpoint", case: 1, value: 28 });
  }, 15000);

  it("shows a refused key request as NEEDS_KEY, never as an approval", async () => {
    const m = await mgmtd(() => ({ ok: true, verdict: "NEEDS_KEY", rule: "G5", case: 0, applied: null, reason: "store key required" }));
    const can: Capabilities = { playback: false, sim: false, live: [], control: ["protected"] };
    const hub = new Hub({ kind: "hello", source: "live", stores: ["protected"], can });
    const op = new Operator([{ store: "protected", host: "127.0.0.1", port: m.port }]);
    cleanup.push(() => op.close());
    new Controller(hub, { root: ROOT, can, rate: 1, operator: op });
    const { msgs, send } = await client(hub);

    send({ kind: "command", id: "c2", store: "protected", op: "disable_alarms", args: {} });
    await until(() => msgs.some((x) => x.kind === "command_result"));
    expect(last(msgs, "command_result")!.reply).toMatchObject({ verdict: "NEEDS_KEY", rule: "G5" });
    // Nothing in the reply can be read as an approval.
    expect(last(msgs, "command_result")!.reply.applied).toBeNull();
  }, 15000);

  it("reports an unreachable target as an error reply instead of throwing", async () => {
    const can: Capabilities = { playback: false, sim: false, live: [], control: ["legacy"] };
    const hub = new Hub({ kind: "hello", source: "live", stores: ["legacy"], can });
    // Port 1 is not listening: connect fails fast.
    const op = new Operator([{ store: "legacy", host: "127.0.0.1", port: 1 }]);
    cleanup.push(() => op.close());
    new Controller(hub, { root: ROOT, can, rate: 1, operator: op });
    const { msgs, send } = await client(hub);

    send({ kind: "command", id: "c3", store: "legacy", op: "stop_defrost", args: { case: 1 } });
    await until(() => msgs.some((x) => x.kind === "command_result"), 15000);
    expect(last(msgs, "command_result")!.reply).toMatchObject({ ok: false });
    expect(last(msgs, "command_result")!.reply.error).toBeTruthy();
  }, 20000);

  it("does not reach mgmtd when the store is not in the control capability", async () => {
    const can: Capabilities = { playback: false, sim: false, live: [], control: [] };
    const hub = new Hub({ kind: "hello", source: "live", stores: ["protected"], can });
    new Controller(hub, { root: ROOT, can, rate: 1 });
    const { msgs, send } = await client(hub);
    send({ kind: "command", id: "c4", store: "protected", op: "disable_alarms", args: {} });
    await new Promise((r) => setTimeout(r, 300));
    expect(msgs.some((x) => x.kind === "command_result")).toBe(false);
    expect(last(msgs, "log")?.text).toContain("no control channel");
  });
});

describe("operator argument validation", () => {
  it("accepts the five operator commands and rejects incomplete arguments", () => {
    expect(validateCommand("set_setpoint", { case: 1, value: 4.5 })).toBeNull();
    expect(validateCommand("start_defrost", { case: 1, seconds: 3600 })).toBeNull();
    expect(validateCommand("stop_defrost", { case: 1 })).toBeNull();
    expect(validateCommand("set_fans", { case: 1, on: false })).toBeNull();
    expect(validateCommand("disable_alarms", {})).toBeNull();

    expect(validateCommand("set_setpoint", { case: 1 })).toMatch(/needs a value/);
    expect(validateCommand("set_setpoint", { case: 1, value: Infinity })).toMatch(/needs a value/);
    expect(validateCommand("start_defrost", { case: 1 })).toMatch(/needs seconds/);
    expect(validateCommand("set_fans", { case: 1 })).toMatch(/needs on/);
    expect(validateCommand("stop_defrost", {})).toMatch(/needs a case/);
  });
});
