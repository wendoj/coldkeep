// SPDX-License-Identifier: Apache-2.0
//
// WebSocket hub: every dashboard gets a hello, then the backlog, then live events.
import { createServer, type Server } from "node:http";
import { WebSocketServer, type WebSocket } from "ws";
import type { Event, Mode, RelayMessage } from "@coldkeep/shared";
import type { Sink } from "./sources";

const BACKLOG_MAX = 50000;

export class Hub implements Sink {
  readonly http: Server;
  private wss: WebSocketServer;
  private backlog: string[] = [];
  private listeners: ((store: Mode, ev: Event) => void)[] = [];

  constructor(private hello: Extract<RelayMessage, { kind: "hello" }>) {
    this.http = createServer((req, res) => {
      res.writeHead(200, { "content-type": "application/json", "access-control-allow-origin": "*" });
      res.end(JSON.stringify({ ...this.hello, events: this.backlog.length }));
    });
    this.wss = new WebSocketServer({ server: this.http });
    this.wss.on("connection", (ws: WebSocket) => {
      ws.send(JSON.stringify(this.hello));
      for (const m of this.backlog) ws.send(m);
    });
  }

  onEvent(fn: (store: Mode, ev: Event) => void) {
    this.listeners.push(fn);
  }

  private send(msg: RelayMessage, keep: boolean) {
    const s = JSON.stringify(msg);
    if (keep) {
      this.backlog.push(s);
      if (this.backlog.length > BACKLOG_MAX) this.backlog.splice(0, this.backlog.length - BACKLOG_MAX);
    }
    for (const c of this.wss.clients) if (c.readyState === c.OPEN) c.send(s);
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
