// SPDX-License-Identifier: Apache-2.0
//
// WebSocket hub: every dashboard gets a hello, then the backlog, then the current
// playback and run state, then live events. Dashboards may send ClientMessages;
// anything that does not parse is ignored.
import { createServer, type Server } from "node:http";
import { WebSocketServer, type WebSocket } from "ws";
import { ClientMessage, type Event, type Mode, type RelayMessage } from "@coldkeep/shared";
import type { Sink } from "./sources";

const BACKLOG_MAX = 50000;
const LOG_MAX = 200;

type Hello = Extract<RelayMessage, { kind: "hello" }>;
type Sticky = Extract<RelayMessage, { kind: "playback" | "run" }>;

export class Hub implements Sink {
  readonly http: Server;
  private wss: WebSocketServer;
  private backlog: string[] = [];
  private logs: string[] = [];
  private sticky = new Map<Sticky["kind"], string>();
  private listeners: ((store: Mode, ev: Event) => void)[] = [];
  private handlers: ((msg: ClientMessage) => void)[] = [];

  constructor(private hello: Hello) {
    this.http = createServer((req, res) => {
      res.writeHead(200, { "content-type": "application/json", "access-control-allow-origin": "*" });
      res.end(JSON.stringify({ ...this.hello, events: this.backlog.length }));
    });
    this.wss = new WebSocketServer({ server: this.http, maxPayload: 4096 });
    this.wss.on("connection", (ws: WebSocket) => {
      ws.send(JSON.stringify(this.hello));
      for (const m of this.backlog) ws.send(m);
      for (const m of this.logs) ws.send(m);
      for (const m of this.sticky.values()) ws.send(m);
      ws.on("message", (data) => {
        let raw: unknown;
        try {
          raw = JSON.parse(String(data));
        } catch {
          return;
        }
        const r = ClientMessage.safeParse(raw);
        if (r.success) for (const h of this.handlers) h(r.data);
      });
    });
  }

  onEvent(fn: (store: Mode, ev: Event) => void) {
    this.listeners.push(fn);
  }

  onClient(fn: (msg: ClientMessage) => void) {
    this.handlers.push(fn);
  }

  private broadcast(s: string) {
    for (const c of this.wss.clients) if (c.readyState === c.OPEN) c.send(s);
  }

  private send(msg: RelayMessage, keep: boolean) {
    const s = JSON.stringify(msg);
    if (keep) {
      this.backlog.push(s);
      if (this.backlog.length > BACKLOG_MAX) this.backlog.splice(0, this.backlog.length - BACKLOG_MAX);
    }
    this.broadcast(s);
  }

  /** Start over: a new hello and an empty backlog. Dashboards drop their state on hello. */
  reset(hello: Hello) {
    this.hello = hello;
    this.backlog = [];
    this.sticky.delete("playback");
    this.broadcast(JSON.stringify(hello));
  }

  get capabilities() {
    return this.hello.can;
  }

  event(store: Mode, event: Event) {
    for (const fn of this.listeners) fn(store, event);
    this.send({ kind: "event", store, event }, true);
  }

  status(store: Mode, connected: boolean, detail?: string) {
    this.send({ kind: "status", store, connected, detail }, false);
  }

  end(store: Mode) {
    this.send({ kind: "end", store }, true);
  }

  /** Latest playback or run state; replayed to dashboards that connect later. */
  state(msg: Sticky) {
    const s = JSON.stringify(msg);
    this.sticky.set(msg.kind, s);
    this.broadcast(s);
  }

  /** One operator command reply: live only, not kept in the backlog. */
  command(msg: Extract<RelayMessage, { kind: "command_result" }>) {
    this.send(msg, false);
  }

  /** One transcript line (attack runner, simulator runs), kept for late joiners. */
  log(text: string) {
    const s = JSON.stringify({ kind: "log", text } satisfies RelayMessage);
    this.logs.push(s);
    if (this.logs.length > LOG_MAX) this.logs.splice(0, this.logs.length - LOG_MAX);
    this.broadcast(s);
  }

  listen(port: number, host = "127.0.0.1"): Promise<number> {
    return new Promise((res) =>
      this.http.listen(port, host, () => {
        const a = this.http.address();
        res(typeof a === "object" && a ? a.port : port);
      }),
    );
  }

  close() {
    for (const c of this.wss.clients) c.terminate();
    this.wss.close();
    this.http.close();
  }
}
