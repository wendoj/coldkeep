// SPDX-License-Identifier: Apache-2.0
import { mkdtempSync, readFileSync } from "node:fs";
import { createServer, type AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import WebSocket from "ws";
import { RelayMessage, parseLine, type Event, type Mode } from "@coldkeep/shared";
import { Hub } from "../src/hub";
import { Recorder } from "../src/recorder";
import { lineSplitter, liveSource, loadRecording, replaySource, type Sink } from "../src/sources";

const ROOT = join(import.meta.dirname, "../../..");
const RUN = (m: Mode) => join(ROOT, `docs/evidence/runs/qemu-run1.${m}.jsonl`);

function collector() {
  const got: { store: Mode; ev: Event }[] = [];
  const ended: Mode[] = [];
  const sink: Sink = { event: (store, ev) => got.push({ store, ev }), status: () => {}, end: (s) => ended.push(s) };
  return { got, ended, sink };
}

const until = async (cond: () => boolean, ms = 3000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error("timeout");
    await new Promise((r) => setTimeout(r, 10));
  }
};

describe("line splitter", () => {
  it("joins chunks and splits lines", () => {
    const lines: string[] = [];
    const feed = lineSplitter((l) => lines.push(l));
    feed('{"a":1}\n{"b"');
    feed(':2}\n\n{"c":3');
    expect(lines).toEqual(['{"a":1}', '{"b":2}']);
  });
});

describe("replay", () => {
  it("replays both recordings in sim-time order, meta first and summary last", async () => {
    const files = [loadRecording(RUN("legacy")), loadRecording(RUN("protected"))];
    const { got, ended, sink } = collector();
    replaySource(files, Infinity, sink, 5);
    await until(() => ended.length === 2);
    for (const m of ["legacy", "protected"] as const) {
      const evs = got.filter((g) => g.store === m).map((g) => g.ev);
      expect(evs.length).toBe(files.find((f) => f.store === m)!.events.length);
      expect(evs[0]!.type).toBe("meta");
      expect(evs.at(-1)!.type).toBe("summary");
    }
  });

  it("paces events by sim time", async () => {
    const files = [loadRecording(RUN("protected"))];
    const { got, sink } = collector();
    const stop = replaySource(files, 600, sink, 20); // 600 sim-s per wall-s
    await new Promise((r) => setTimeout(r, 250));
    stop();
    const ts = got.map((g) => ("t" in g.ev ? (g.ev.t as number) : 0));
    const first = files[0]!.events.find((e) => e.type === "telemetry")! as { t: number };
    expect(Math.max(...ts) - first.t).toBeLessThan(600 * 0.6);
    expect(got.some((g) => g.ev.type === "summary")).toBe(false);
  });
});

describe("live bridge source", () => {
  it("turns bridge audit records into decisions and reconnects", async () => {
    let conns = 0;
    const server = createServer((sock) => {
      conns++;
      sock.write('{"type":"meta","source":"qnx-bridge","mode":"protected","seed":7,"speed":30,"cases":[{"id":1,"type":"chilled","limit":8}]}\n');
      sock.write('{"type":"telemetry","t":601,"cases":[{"id":1,"type":"chilled","air":3,"prod":3,"sp":3,"comp":1,"heat":0,"defrost":0,"door":0,"hold":0}]}\n');
      sock.write('{"type":"audit","seq":4,"text":"t=601 mgmt/admin start_defrost case=1 value=86400.0 -> CLAMP G3 max defrost 45 min"}\n');
      if (conns === 1) setTimeout(() => sock.destroy(), 50);
    });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const port = (server.address() as AddressInfo).port;
    const { got, sink } = collector();
    const stop = liveSource("protected", "127.0.0.1", port, sink);
    await until(() => got.filter((g) => g.ev.type === "decision").length >= 2, 6000);
    stop();
    server.close();
    const d = got.find((g) => g.ev.type === "decision")!.ev;
    expect(d).toMatchObject({ type: "decision", verdict: "CLAMP", rule: "G3", case: 1 });
    expect(conns).toBe(2);
  }, 10000);
});

describe("recorder", () => {
  it("writes a self-describing recording with a summary computed from samples", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ck-rec-"));
    const rec = new Recorder(dir, "t1", "abc123");
    rec.record("legacy", { type: "telemetry", t: 1, cases: [] }); // before meta: dropped
    rec.record("legacy", { type: "meta", mode: "legacy", cases: [{ id: 1, type: "chilled", limit: 8 }] });
    rec.record("legacy", { type: "meta", mode: "legacy" }); // reconnect: dropped
    const s = (prod: number) => ({ id: 1, air: prod, prod, sp: 3, comp: 0, heat: 0, defrost: 0, door: 0, hold: 0 });
    rec.record("legacy", { type: "telemetry", t: 0, cases: [s(3)] });
    rec.record("legacy", { type: "telemetry", t: 120, cases: [s(9)] });
    rec.record("legacy", { type: "decision", t: 100, cmd: "set_setpoint", verdict: "ALLOW", rule: "LEGACY" });
    const [path] = await rec.close();
    const lines = readFileSync(path!, "utf8").trim().split("\n");
    const evs = lines.map((l) => {
      const r = parseLine(l);
      if ("error" in r) throw new Error(r.error);
      return r.event;
    });
    expect(evs.map((e) => e.type)).toEqual(["meta", "telemetry", "telemetry", "decision", "summary"]);
    expect(evs[0]).toMatchObject({ label: "t1", git: "abc123" });
    expect(evs[4]).toMatchObject({ minutes_above_total: 2, food_at_risk_kg: 400, a1_applied: 1, a1_blocked: 0 });
  });
});

describe("websocket hub", () => {
  it("sends hello and the backlog to a late client", async () => {
    const hub = new Hub({ kind: "hello", source: "replay", stores: ["legacy"] });
    const port = await hub.listen(0);
    hub.event("legacy", { type: "note", t: 1, text: "before" });
    const msgs: RelayMessage[] = [];
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);
    ws.on("message", (d) => msgs.push(RelayMessage.parse(JSON.parse(d.toString()))));
    await until(() => msgs.length >= 2);
    hub.event("legacy", { type: "note", t: 2, text: "after" });
    await until(() => msgs.length >= 3);
    ws.close();
    hub.close();
    expect(msgs.map((m) => m.kind)).toEqual(["hello", "event", "event"]);
  });
});
