// SPDX-License-Identifier: Apache-2.0
//
// Client for mgmtd's documented JSON-lines API (target/mgmtd, core/proto.c).
import { createConnection, type Socket } from "node:net";
import { z } from "zod";

export const StatusCase = z.looseObject({ id: z.number(), type: z.string(), sp: z.number() });
export const Status = z.looseObject({ ok: z.literal(true), mode: z.string(), t: z.number(), cases: z.array(StatusCase) });
export type Status = z.infer<typeof Status>;

export const Reply = z.looseObject({
  ok: z.boolean(),
  id: z.number().optional(),
  cmd: z.string().optional(),
  case: z.number().optional(),
  verdict: z.string().optional(),
  rule: z.string().optional(),
  applied: z.number().nullable().optional(),
  reason: z.string().optional(),
  error: z.string().optional(),
});
export type Reply = z.infer<typeof Reply>;

export class MgmtClient {
  private sock: Socket | null = null;
  private buf = "";
  private waiting: ((line: string) => void)[] = [];

  constructor(
    readonly host: string,
    readonly port: number,
  ) {}

  connect(timeoutMs = 10000): Promise<void> {
    return new Promise((resolve, reject) => {
      const s = createConnection({ host: this.host, port: this.port });
      const timer = setTimeout(() => {
        s.destroy();
        reject(new Error(`${this.host}:${this.port}: connect timeout`));
      }, timeoutMs);
      s.setNoDelay(true);
      s.once("connect", () => {
        clearTimeout(timer);
        this.sock = s;
        // a reset after connect fails the pending requests instead of crashing the process
        s.on("error", () => {});
        s.on("close", () => {
          for (const w of this.waiting.splice(0)) w('{"ok":false,"error":"connection closed"}');
        });
        resolve();
      });
      s.once("error", (e) => {
        clearTimeout(timer);
        reject(e);
      });
      s.on("data", (d) => {
        this.buf += d.toString();
        let nl: number;
        while ((nl = this.buf.indexOf("\n")) >= 0) {
          const line = this.buf.slice(0, nl);
          this.buf = this.buf.slice(nl + 1);
          this.waiting.shift()?.(line);
        }
      });
    });
  }

  /** Send one request and wait for its reply line. */
  request(obj: object, timeoutMs = 30000): Promise<unknown> {
    const s = this.sock;
    if (!s) return Promise.reject(new Error("not connected"));
    return new Promise((resolve, reject) => {
      const waiter = (line: string) => {
        clearTimeout(timer);
        try {
          resolve(JSON.parse(line));
        } catch (e) {
          reject(e);
        }
      };
      const timer = setTimeout(() => {
        // Drop the waiter, or every later reply would be matched one request too early.
        const i = this.waiting.indexOf(waiter);
        if (i >= 0) this.waiting.splice(i, 1);
        reject(new Error("reply timeout"));
      }, timeoutMs);
      this.waiting.push(waiter);
      s.write(JSON.stringify(obj) + "\n", (err) => {
        if (!err) return;
        clearTimeout(timer);
        const i = this.waiting.indexOf(waiter);
        if (i >= 0) this.waiting.splice(i, 1);
        reject(err);
      });
    });
  }

  /** Fire a request without waiting (A2 flood); replies are counted by drain(). */
  sendNoWait(obj: object): boolean {
    let ok = true;
    this.waiting.push(() => {});
    if (this.sock) ok = this.sock.write(JSON.stringify(obj) + "\n");
    return ok;
  }

  get pending(): number {
    return this.waiting.length;
  }

  async login(user = "admin", pass = "admin"): Promise<Reply> {
    return Reply.parse(await this.request({ op: "login", user, pass }));
  }

  async status(): Promise<Status> {
    return Status.parse(await this.request({ op: "status" }));
  }

  async command(op: string, args: Record<string, number | boolean> = {}): Promise<Reply> {
    return Reply.parse(await this.request({ op, ...args }));
  }

  close() {
    this.sock?.destroy();
    this.sock = null;
  }
}
