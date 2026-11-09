// SPDX-License-Identifier: Apache-2.0
//
// Runs campaigns against one or two stores at scripted sim times and prints the
// transcript line by line, e.g.
//   [A1] t=601 start_defrost case=1 seconds=86400 -> legacy: APPLIED | coldkeep: CLAMP (G3 max defrost 45 min)
import type { Mode } from "@coldkeep/shared";
import { MgmtClient, type Reply } from "./client";
import { a1Steps, describe, flood, type FloodResult } from "./campaigns";

export interface Target {
  store: Mode;
  host: string;
  port: number;
}

export interface RunOptions {
  a1At: number | null;   // sim seconds, null = skip
  a2At: number | null;
  a2WallMs: number;
  pace: number;          // wall ms between A1 commands
  pollMs: number;
  log: (line: string) => void;
}

export interface TranscriptLine {
  campaign: string;
  t: number;
  op: string;
  args: Record<string, number>;
  replies: Partial<Record<Mode, Reply>>;
}

const label = (m: Mode) => (m === "legacy" ? "legacy" : "coldkeep");
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function waitForSim(c: MgmtClient, t: number, pollMs: number): Promise<number> {
  for (;;) {
    const st = await c.status();
    if (st.t >= t) return st.t;
    await sleep(pollMs);
  }
}

export async function run(targets: Target[], o: RunOptions): Promise<{ lines: TranscriptLine[]; floods: Partial<Record<Mode, FloodResult>> }> {
  const clients = new Map<Mode, MgmtClient>();
  for (const t of targets) {
    const c = new MgmtClient(t.host, t.port);
    await c.connect();
    const r = await c.login();
    if (!r.ok) throw new Error(`${t.store}: login failed: ${r.error ?? "?"}`);
    clients.set(t.store, c);
    o.log(`logged in to ${label(t.store)} (${t.host}:${t.port}) as admin/admin`);
  }
  const first = clients.get(targets[0]!.store)!;
  const lines: TranscriptLine[] = [];
  const floods: Partial<Record<Mode, FloodResult>> = {};

  if (o.a1At !== null) {
    await waitForSim(first, o.a1At, o.pollMs);
    // the same steps for every store, from the first store's case list
    const steps = a1Steps(await first.status());
    for (const s of steps) {
      const line: TranscriptLine = { campaign: s.campaign, t: 0, op: s.op, args: s.args, replies: {} };
      await Promise.all(
        [...clients].map(async ([m, c]) => {
          line.replies[m] = await c.command(s.op, s.args);
        }),
      );
      line.t = (await first.status()).t;
      lines.push(line);
      const args = Object.entries(s.args).map(([k, v]) => `${k}=${v}`).join(" ");
      o.log(`[${s.campaign}] t=${line.t.toFixed(0)} ${s.op}${args ? " " + args : ""} -> ` +
        targets.map((t) => `${label(t.store)}: ${describe(line.replies[t.store]!)}`).join(" | "));
      await sleep(o.pace);
    }
  }

  if (o.a2At !== null) {
    await waitForSim(first, o.a2At, o.pollMs);
    // one extra connection per store so the flood does not share the transcript socket
    await Promise.all(
      targets.map(async (t) => {
        const c = new MgmtClient(t.host, t.port);
        await c.connect();
        await c.login();
        floods[t.store] = await flood(c, o.a2WallMs);
        c.close();
      }),
    );
    o.log(`[A2] flood for ${(o.a2WallMs / 1000).toFixed(0)} s wall -> ` +
      targets.map((t) => {
        const f = floods[t.store]!;
        return `${label(t.store)}: ${f.requests} requests, ${f.replies} answered (${Math.round((f.replies * 1000) / f.wallMs)}/s)`;
      }).join(" | "));
  }

  for (const c of clients.values()) c.close();
  return { lines, floods };
}
