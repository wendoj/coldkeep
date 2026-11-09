// SPDX-License-Identifier: Apache-2.0
//
//   npm run attack -- --store legacy=127.0.0.1:18081 --store protected=127.0.0.1:18080 \
//                     [--a1-at 600] [--a2-at 1500 --a2-seconds 10] [--pace-ms 1000]
//
// The QEMU slots forward mgmtd's port 8080 to 18080 + slot (scripts/qemu-run.sh).
import { parseArgs } from "node:util";
import { Mode } from "@coldkeep/shared";
import { run, type Target } from "./runner";

const { values } = parseArgs({
  options: {
    store: { type: "string", multiple: true, default: [] },
    "a1-at": { type: "string", default: "600" },
    "a2-at": { type: "string" },
    "a2-seconds": { type: "string", default: "10" },
    "pace-ms": { type: "string", default: "1000" },
    "poll-ms": { type: "string", default: "250" },
    "no-a1": { type: "boolean", default: false },
  },
});

const targets: Target[] = values.store.map((s) => {
  const m = /^(legacy|protected)=([^:]+):(\d+)$/.exec(s);
  if (!m) throw new Error(`--store ${s}: expected legacy=host:port or protected=host:port`);
  return { store: Mode.parse(m[1]), host: m[2]!, port: Number(m[3]) };
});
if (!targets.length) {
  console.error("usage: attack --store legacy=127.0.0.1:18081 --store protected=127.0.0.1:18080 [--a1-at 600] [--a2-at 1500]");
  process.exit(2);
}

run(targets, {
  a1At: values["no-a1"] ? null : Number(values["a1-at"]),
  a2At: values["a2-at"] !== undefined ? Number(values["a2-at"]) : null,
  a2WallMs: Number(values["a2-seconds"]) * 1000,
  pace: Number(values["pace-ms"]),
  pollMs: Number(values["poll-ms"]),
  log: (l) => console.log(l),
}).catch((e) => {
  console.error(`attack: ${(e as Error).message}`);
  process.exit(1);
});
