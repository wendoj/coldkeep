// SPDX-License-Identifier: Apache-2.0
//
// COLDKEEP host relay: target bridges, recordings or the host simulator -> WebSocket
// for the dashboard.
//
//   sim:     relay [--sim] [--seed 7] [--rate 60]
//            Runs every campaign in the host simulator, then lets the dashboard
//            start new runs with chosen campaigns and seed.
//   replay:  relay --replay a.legacy.jsonl --replay a.protected.jsonl [--rate 120]
//   live:    relay --store legacy=127.0.0.1:17071 --store protected=127.0.0.1:17070
//                  [--mgmt legacy=127.0.0.1:18081 --mgmt protected=127.0.0.1:18080]
//                  [--record run1] [--end 3600]
//            With --mgmt the dashboard can send A1 and A2 to the targets' mgmtd.
//
// Options: --port 7071 (WebSocket + JSON hello over HTTP), --out ../recordings,
//          --end <sim s>: in live mode, stop (and close the recording) once every store reaches it
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { Mode, type Campaign } from "@coldkeep/shared";
import type { Target } from "@coldkeep/attack/src/runner";
import { Controller } from "./control";
import { Hub } from "./hub";
import { Operator } from "./operator";
import { Recorder, gitSha } from "./recorder";
import { liveSource, loadRecording } from "./sources";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
// `npm run relay` runs this from host/relay; resolve paths from where the user ran npm
const CWD = process.env.INIT_CWD ?? process.cwd();
const fromCwd = (p: string) => resolve(CWD, p);
const ALL: Campaign[] = ["A1", "A2", "A3", "A4"];

const { values } = parseArgs({
  options: {
    sim: { type: "boolean", default: false },
    seed: { type: "string", default: "7" },
    store: { type: "string", multiple: true, default: [] },
    mgmt: { type: "string", multiple: true, default: [] },
    replay: { type: "string", multiple: true, default: [] },
    rate: { type: "string" },
    port: { type: "string", default: "7071" },
    record: { type: "string" },
    out: { type: "string", default: join(ROOT, "recordings") },
    end: { type: "string" },
  },
});

function endpoints(flag: string, list: string[]): Target[] {
  return list.map((s) => {
    const m = /^(legacy|protected)=([^:]+):(\d+)$/.exec(s);
    if (!m) throw new Error(`--${flag} ${s}: expected legacy=host:port or protected=host:port`);
    return { store: Mode.parse(m[1]), host: m[2]!, port: Number(m[3]) };
  });
}

async function main() {
  const stops: (() => void)[] = [];
  let recorder: Recorder | null = null;
  let operator: Operator | null = null;
  let hub: Hub;
  let shutdown = async () => {};
  const simOk = existsSync(join(ROOT, "sim-host/Makefile"));
  const rateArg = (fallback: number) => (values.rate === "max" ? Infinity : Number(values.rate ?? fallback));

  if (values.replay.length) {
    const files = values.replay.map((p) => loadRecording(fromCwd(p)));
    const can = { playback: true, sim: simOk, live: [] };
    hub = new Hub({ kind: "hello", source: "replay", stores: files.map((f) => f.store), files: files.map((f) => f.path), can });
    const port = await hub.listen(Number(values.port));
    const rate = rateArg(120);
    console.log(`relay: replaying ${files.length} recording(s) at ${rate}x sim time on ws://127.0.0.1:${port}`);
    new Controller(hub, { root: ROOT, can, rate }).play(files, "replay");
  } else if (values.store.length) {
    const targets = endpoints("store", values.store);
    const mgmt = endpoints("mgmt", values.mgmt);
    const can = {
      playback: false,
      sim: false,
      live: mgmt.length ? (["A1", "A2"] as Campaign[]) : [],
      control: mgmt.map((t) => t.store),
    };
    hub = new Hub({ kind: "hello", source: "live", stores: targets.map((t) => t.store), can });
    operator = mgmt.length ? new Operator(mgmt) : null;
    new Controller(hub, { root: ROOT, can, rate: 1, mgmt, operator: operator ?? undefined });
    const reached = new Set<string>(); // stores past --end: nothing more is recorded for them
    if (values.record) {
      recorder = new Recorder(fromCwd(values.out), values.record, gitSha(ROOT));
      const r = recorder;
      hub.onEvent((store, ev) => {
        if (!reached.has(store)) r.record(store, ev);
      });
    }
    if (values.end !== undefined) {
      const end = Number(values.end);
      hub.onEvent((store, ev) => {
        if (ev.type !== "telemetry" || ev.t < end || reached.has(store)) return;
        reached.add(store);
        if (reached.size === targets.length) {
          console.log(`relay: every store reached sim t=${end}`);
          void shutdown();
        }
      });
    }
    const port = await hub.listen(Number(values.port));
    console.log(`relay: live from ${targets.map((t) => `${t.store}=${t.host}:${t.port}`).join(", ")} on ws://127.0.0.1:${port}`);
    if (mgmt.length) console.log(`relay: dashboard may send A1/A2 to ${mgmt.map((t) => `${t.store}=${t.host}:${t.port}`).join(", ")}`);
    for (const t of targets) stops.push(liveSource(t.store, t.host, t.port, hub));
  } else {
    if (!simOk) throw new Error("no sim-host/ in this checkout: use --replay or --store");
    const can = { playback: true, sim: true, live: [] };
    hub = new Hub({ kind: "hello", source: "sim", stores: [], can });
    const port = await hub.listen(Number(values.port));
    const rate = rateArg(60);
    console.log(`relay: host simulator (not a QNX measurement) at ${rate}x sim time on ws://127.0.0.1:${port}`);
    await new Controller(hub, { root: ROOT, can, rate }).runSim(ALL, Number(values.seed));
  }

  shutdown = async () => {
    for (const s of stops) s();
    operator?.close();
    if (recorder) for (const p of await recorder.close()) console.log(`relay: recorded ${p}`);
    hub.close();
    process.exit(0);
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

main().catch((e) => {
  console.error(`relay: ${(e as Error).message}`);
  process.exit(1);
});
