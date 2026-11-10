// SPDX-License-Identifier: Apache-2.0
//
// Runs a comparison in the host simulator (sim-host/ck-sim) for the dashboard.
// The relay writes the scenario itself from a fixed template, so a dashboard can
// only choose which campaigns run and the seed: no paths and no free text.
import { execFile } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";
import type { Campaign } from "@coldkeep/shared";
import { loadRecording, type ReplayFile } from "./sources";

const run = promisify(execFile);

/** Sim times as in scenarios/demo-main.yaml; one sim hour in total. */
export const CAMPAIGN_AT: Record<Campaign, number> = { A1: 600, A2: 1500, A3: 2100, A4: 2700 };
export const SIM_END = 3600;

export function scenarioYaml(campaigns: Campaign[], seed: number): string {
  const picked = [...new Set(campaigns)].sort((a, b) => CAMPAIGN_AT[a] - CAMPAIGN_AT[b]);
  return [
    "name: dashboard",
    `seed: ${seed}`,
    "speed: 30",
    "cases: [chilled x4, frozen x2, coldroom x2]",
    "events:",
    "  - { t: 0, do: baseline }",
    ...picked.map((c) => `  - { t: ${CAMPAIGN_AT[c]}, do: attack, campaign: ${c} }`),
    `  - { t: ${SIM_END}, do: end }`,
    "",
  ].join("\n");
}

async function binary(root: string): Promise<string> {
  const bin = join(root, "sim-host/ck-sim");
  if (!existsSync(bin)) await run("make", ["-C", join(root, "sim-host")]);
  return bin;
}

export interface SimRun {
  files: ReplayFile[];
  /** ck-sim's attack transcript, one line per command, with its sim time. */
  transcript: { t: number; text: string }[];
}

/** Run both stores in ck-sim and load the two recordings it writes. */
export async function simulate(root: string, campaigns: Campaign[], seed: number): Promise<SimRun> {
  const bin = await binary(root);
  const dir = mkdtempSync(join(tmpdir(), "coldkeep-sim-"));
  try {
    const scen = join(dir, "dashboard.yaml");
    writeFileSync(scen, scenarioYaml(campaigns, seed));
    const { stdout } = await run(bin, ["--scenario", scen, "--mode", "both", "--out", dir], { timeout: 60_000 });
    const transcript = stdout.split("\n").flatMap((line) => {
      const m = /^\[A\d\] t=(\d+(?:\.\d+)?) /.exec(line);
      return m ? [{ t: Number(m[1]), text: line }] : [];
    });
    const files = [loadRecording(join(dir, "dashboard.legacy.jsonl")), loadRecording(join(dir, "dashboard.protected.jsonl"))];
    for (const f of files) f.path = `host simulator, seed ${seed}`;
    return { files, transcript };
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}
