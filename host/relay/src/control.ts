// SPDX-License-Identifier: Apache-2.0
//
// Handles dashboard requests: playback (pause, resume, restart, rate), new
// host-simulator runs, and campaigns against live targets' management API.
// Key requests are never approved from here: that stays at the store's console.
import type { Campaign, Capabilities, ClientMessage, Rate } from "@coldkeep/shared";
import { run as attack, type Target } from "@coldkeep/attack/src/runner";
import type { Hub } from "./hub";
import type { Operator } from "./operator";
import { simulate } from "./sim";
import { Playback, type ReplayFile } from "./sources";

export interface ControlOptions {
  root: string;
  can: Capabilities;
  rate: number;
  /** management API endpoints for live campaigns and operator commands */
  mgmt?: Target[];
  /** live operator commands; only present when the relay has --mgmt endpoints */
  operator?: Operator;
}

const toRate = (r: Rate) => (r === "max" ? Infinity : r);
const fromRate = (r: number): Rate => (r === Infinity ? "max" : r);

export class Controller {
  private playback: Playback | null = null;
  private files: ReplayFile[] = [];
  private source: "replay" | "sim" = "replay";
  private transcript: { t: number; text: string }[] = [];
  private shown = 0;
  private rate: number;
  private busy = false;

  constructor(
    private hub: Hub,
    private o: ControlOptions,
  ) {
    this.rate = o.rate;
    hub.onClient((m) => void this.handle(m));
    // release the simulator's transcript lines as playback reaches their sim time
    hub.onEvent((_store, ev) => {
      if (ev.type !== "telemetry") return;
      while (this.shown < this.transcript.length && this.transcript[this.shown]!.t <= ev.t) hub.log(this.transcript[this.shown++]!.text);
    });
  }

  private publish() {
    const p = this.playback;
    this.hub.state({ kind: "playback", paused: p?.paused ?? false, rate: fromRate(this.rate), done: p?.done ?? false });
  }

  /** Replace whatever is playing with these recordings, from the start. */
  play(files: ReplayFile[], source: "replay" | "sim", transcript: { t: number; text: string }[] = []) {
    this.playback?.stop();
    this.files = files;
    this.source = source;
    this.transcript = transcript;
    this.shown = 0;
    this.hub.reset({
      kind: "hello",
      source,
      stores: files.map((f) => f.store),
      files: files.map((f) => f.path),
      can: this.o.can,
    });
    this.playback = new Playback(files, this.rate, this.hub, 50, () => this.publish());
    this.publish();
  }

  async runSim(campaigns: Campaign[], seed: number) {
    const run = { target: "sim" as const, campaigns, seed };
    this.busy = true;
    this.hub.state({ kind: "run", run: { ...run, state: "running" } });
    try {
      const r = await simulate(this.o.root, campaigns, seed);
      this.hub.log(`host simulator: ${campaigns.join(" + ")}, seed ${seed}`);
      this.play(r.files, "sim", r.transcript);
      this.hub.state({ kind: "run", run: { ...run, state: "done" } });
    } catch (e) {
      this.hub.state({ kind: "run", run: { ...run, state: "error", detail: (e as Error).message } });
    } finally {
      this.busy = false;
    }
  }

  private async runLive(campaigns: Campaign[]) {
    const allowed = campaigns.filter((c) => this.o.can.live.includes(c));
    const run = { target: "live" as const, campaigns: allowed };
    if (!allowed.length || !this.o.mgmt?.length) return;
    this.busy = true;
    this.hub.state({ kind: "run", run: { ...run, state: "running" } });
    try {
      // a1At / a2At = 0: start as soon as the stores answer
      await attack(this.o.mgmt, {
        a1At: allowed.includes("A1") ? 0 : null,
        a2At: allowed.includes("A2") ? 0 : null,
        a2WallMs: 10_000,
        pace: 1000,
        pollMs: 250,
        log: (l) => this.hub.log(l),
      });
      this.hub.state({ kind: "run", run: { ...run, state: "done" } });
    } catch (e) {
      this.hub.state({ kind: "run", run: { ...run, state: "error", detail: (e as Error).message } });
    } finally {
      this.busy = false;
    }
  }

  async handle(m: ClientMessage) {
    const p = this.playback;
    if (m.kind === "playback" && this.o.can.playback && p) {
      if (m.action === "pause") p.pause();
      else if (m.action === "resume") p.resume();
      else this.play(this.files, this.source, this.transcript);
      this.publish();
    } else if (m.kind === "rate" && this.o.can.playback) {
      this.rate = toRate(m.rate);
      if (p) p.rate = this.rate;
      this.publish();
    } else if (m.kind === "run") {
      if (this.busy) return this.hub.log("a run is already in progress");
      if (m.rate !== undefined && this.o.can.playback) this.rate = toRate(m.rate);
      if (m.target === "sim" && this.o.can.sim) await this.runSim(m.campaigns, m.seed ?? 7);
      else if (m.target === "live") await this.runLive(m.campaigns);
    } else if (m.kind === "command") {
      if (!this.o.can.control?.includes(m.store) || !this.o.operator) {
        return this.hub.log(`operator: no control channel for the ${m.store} store`);
      }
      const at = Date.now();
      const reply = await this.o.operator.command(m.store, m.op, m.args);
      this.hub.command({ kind: "command_result", id: m.id, store: m.store, op: m.op, args: m.args, at, reply });
      console.log(`relay: ${m.store} ${m.op} -> ${reply.verdict ?? (reply.ok ? "OK" : `ERROR ${reply.error ?? ""}`)}`);
    }
  }
}
