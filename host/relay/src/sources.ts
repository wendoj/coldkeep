// SPDX-License-Identifier: Apache-2.0
//
// Event sources for the relay: a live target bridge (TCP JSON lines) or a pair
// of recordings replayed on a sim-time clock.
import { readFileSync } from "node:fs";
import { createConnection, type Socket } from "node:net";
import { parseAuditText, parseLine, type Event, type Mode } from "@coldkeep/shared";

export interface Sink {
  event(store: Mode, ev: Event): void;
  status(store: Mode, connected: boolean, detail?: string): void;
  end(store: Mode): void;
}

/** Split a byte stream into lines. */
export function lineSplitter(onLine: (line: string) => void) {
  let buf = "";
  return (chunk: Buffer | string) => {
    buf += chunk.toString();
    let nl: number;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (line) onLine(line);
    }
  };
}

/** Bridge audit records become ordinary recording events (decision, compliance, note). */
export function normalise(ev: Event): Event | null {
  if (ev.type !== "audit") return ev;
  return parseAuditText(ev.text);
}

/** Connect to one store's bridge and keep reconnecting. Returns a stop function. */
export function liveSource(store: Mode, host: string, port: number, sink: Sink): () => void {
  let sock: Socket | null = null;
  let stopped = false;
  let timer: NodeJS.Timeout | null = null;
  const connect = () => {
    if (stopped) return;
    sock = createConnection({ host, port });
    sock.setNoDelay(true);
    sock.on("connect", () => sink.status(store, true, `${host}:${port}`));
    sock.on(
      "data",
      lineSplitter((line) => {
        const r = parseLine(line);
        if ("error" in r) return sink.status(store, true, `bad line from bridge: ${r.error}`);
        const ev = normalise(r.event);
        if (ev) sink.event(store, ev);
      }),
    );
    sock.on("error", () => {});
    sock.on("close", () => {
      sink.status(store, false, `${host}:${port} disconnected; retrying`);
      if (!stopped) timer = setTimeout(connect, 2000);
    });
  };
  connect();
  return () => {
    stopped = true;
    if (timer) clearTimeout(timer);
    sock?.destroy();
  };
}

export interface ReplayFile {
  store: Mode;
  path: string;
  events: Event[];
}

export function loadRecording(path: string): ReplayFile {
  const events: Event[] = [];
  for (const [i, line] of readFileSync(path, "utf8").split("\n").entries()) {
    if (!line.trim()) continue;
    const r = parseLine(line);
    if ("error" in r) throw new Error(`${path}:${i + 1}: ${r.error}`);
    const ev = normalise(r.event);
    if (ev) events.push(ev);
  }
  const meta = events.find((e) => e.type === "meta");
  if (!meta || meta.type !== "meta") throw new Error(`${path}: no meta line`);
  return { store: meta.mode, path, events };
}

const eventTime = (ev: Event): number | null => ("t" in ev && typeof ev.t === "number" ? ev.t : null);

/**
 * Replays recordings together on one sim clock: `rate` sim-seconds per wall second
 * (Infinity = as fast as possible). meta goes first, summary when the file ends.
 * Can be paused, resumed and re-rated while it runs.
 */
export class Playback {
  private queues: { f: ReplayFile; i: number }[];
  private simNow: number;
  private timer: NodeJS.Timeout | null = null;
  paused = false;
  done = false;

  constructor(
    files: ReplayFile[],
    public rate: number,
    private sink: Sink,
    private tickMs = 50,
    private onDone?: () => void,
  ) {
    this.queues = files.map((f) => ({ f, i: 0 }));
    for (const q of this.queues) {
      sink.status(q.f.store, true, `replay ${q.f.path}`);
      while (q.i < q.f.events.length && eventTime(q.f.events[q.i]!) === null) sink.event(q.f.store, q.f.events[q.i++]!);
    }
    this.simNow = Math.min(...files.map((f) => f.events.map(eventTime).find((t): t is number => t !== null) ?? 0));
    this.timer = setInterval(() => this.step(), tickMs);
    this.step();
  }

  private step() {
    if (this.paused || this.done) return;
    this.simNow = this.rate === Infinity ? Infinity : this.simNow + (this.rate * this.tickMs) / 1000;
    let done = true;
    for (const q of this.queues) {
      while (q.i < q.f.events.length) {
        const ev = q.f.events[q.i]!;
        const t = eventTime(ev);
        if (t !== null && t > this.simNow) break;
        this.sink.event(q.f.store, ev);
        q.i++;
      }
      if (q.i < q.f.events.length) done = false;
      else if (q.i === q.f.events.length) {
        this.sink.end(q.f.store);
        q.i++;
      }
    }
    if (done) {
      this.done = true;
      this.stop();
      this.onDone?.();
    }
  }

  pause() {
    this.paused = true;
  }

  resume() {
    this.paused = false;
  }

  stop() {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }
}

export function replaySource(files: ReplayFile[], rate: number, sink: Sink, tickMs = 50): () => void {
  const p = new Playback(files, rate, sink, tickMs);
  return () => p.stop();
}
