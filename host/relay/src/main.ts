// SPDX-License-Identifier: Apache-2.0
//
// COLDKEEP host relay: target bridges (or recordings) -> WebSocket for the dashboard.
//
//   live:    relay --store legacy=127.0.0.1:17071 --store protected=127.0.0.1:17070 [--record run1]
//   replay:  relay --replay a.legacy.jsonl --replay a.protected.jsonl [--rate 120]
//
// Options: --port 7071 (WebSocket + JSON hello over HTTP), --out ../recordings
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { Mode } from "@coldkeep/shared";
import { Hub } from "./hub";
import { Recorder, gitSha } from "./recorder";
import { liveSource, loadRecording, replaySource } from "./sources";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");

const { values } = parseArgs({
  options: {
    store: { type: "string", multiple: true, default: [] },
    replay: { type: "string", multiple: true, default: [] },
    rate: { type: "string", default: "120" },
    port: { type: "string", default: "7071" },
    record: { type: "string" },
    out: { type: "string", default: join(ROOT, "recordings") },
  },
});

async function main() {
  const stops: (() => void)[] = [];
  let recorder: Recorder | null = null;
  let hub: Hub;

  if (values.replay.length) {
    const files = values.replay.map((p) => loadRecording(resolve(p)));
    const rate = values.rate === "max" ? Infinity : Number(values.rate);
    hub = new Hub({ kind: "hello", source: "replay", stores: files.map((f) => f.store), files: files.map((f) => f.path) });
    const port = await hub.listen(Number(values.port));
    console.log(`relay: replaying ${files.length} recording(s) at ${rate}x sim time on ws://127.0.0.1:${port}`);
    stops.push(replaySource(files, rate, hub));
  } else if (values.store.length) {
    const targets = values.store.map((s) => {
      const m = /^(legacy|protected)=([^:]+):(\d+)$/.exec(s);
      if (!m) throw new Error(`--store ${s}: expected legacy=host:port or protected=host:port`);
      return { store: Mode.parse(m[1]), host: m[2]!, port: Number(m[3]) };
    });
    hub = new Hub({ kind: "hello", source: "live", stores: targets.map((t) => t.store) });
    if (values.record) {
      recorder = new Recorder(resolve(values.out), values.record, gitSha(ROOT));
      const r = recorder;
      hub.onEvent((store, ev) => r.record(store, ev));
    }
    const port = await hub.listen(Number(values.port));
    console.log(`relay: live from ${targets.map((t) => `${t.store}=${t.host}:${t.port}`).join(", ")} on ws://127.0.0.1:${port}`);
    for (const t of targets) stops.push(liveSource(t.store, t.host, t.port, hub));
  } else {
    console.error("usage: relay --store legacy=host:port --store protected=host:port [--record label]\n" +
      "       relay --replay file.legacy.jsonl --replay file.protected.jsonl [--rate 120|max]");
    process.exit(2);
  }

  const shutdown = async () => {
    for (const s of stops) s();
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
