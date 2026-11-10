// SPDX-License-Identifier: Apache-2.0
//
// Operator commands from the dashboard to a live target's mgmtd.
//
// This is deliberately narrow: it exposes only the documented management
// commands an operator uses, never login/status/debug_exec, and it never
// approves a key request. Every command still goes through mgmtd -> brokerd ->
// governord, so the governor decides, exactly as it does for the attacker.
import type { CommandArgs, CommandReply, Mode, OperatorOp } from "@coldkeep/shared";
import { MgmtClient } from "@coldkeep/attack/src/client";
import type { Target } from "@coldkeep/attack/src/runner";

/** Missing required arguments, or null when the command can be sent. */
export function validateCommand(op: OperatorOp, a: CommandArgs): string | null {
  const needsCase = op !== "disable_alarms";
  if (needsCase && (a.case === undefined || a.case < 1)) return `${op} needs a case`;
  switch (op) {
    case "set_setpoint":
      return a.value === undefined || !Number.isFinite(a.value) ? `${op} needs a value` : null;
    case "start_defrost":
      return a.seconds === undefined ? "start_defrost needs seconds" : null;
    case "set_fans":
      return typeof a.on !== "boolean" ? "set_fans needs on" : null;
    case "stop_defrost":
    case "disable_alarms":
      return null;
  }
}

/** The flat object mgmtd's parser expects (core/proto.c). */
function wire(a: CommandArgs): Record<string, number | boolean> {
  const out: Record<string, number | boolean> = {};
  if (a.case !== undefined) out.case = a.case;
  if (a.value !== undefined) out.value = a.value;
  if (a.seconds !== undefined) out.seconds = a.seconds;
  if (a.on !== undefined) out.on = a.on;
  return out;
}

export class Operator {
  private clients = new Map<Mode, MgmtClient>();
  /** one command at a time per store, so reconnects cannot interleave requests */
  private chains = new Map<Mode, Promise<unknown>>();

  constructor(private targets: Target[]) {}

  has(store: Mode): boolean {
    return this.targets.some((t) => t.store === store);
  }

  private serial<T>(store: Mode, fn: () => Promise<T>): Promise<T> {
    const prev = this.chains.get(store) ?? Promise.resolve();
    const next = prev.then(fn, fn);
    this.chains.set(store, next.then(() => undefined, () => undefined));
    return next;
  }

  private async open(t: Target): Promise<MgmtClient> {
    const c = new MgmtClient(t.host, t.port);
    await c.connect();
    try {
      const r = await c.login("admin", "admin");
      if (!r.ok) throw new Error(r.error ?? "login refused");
    } catch (e) {
      c.close();
      throw e;
    }
    return c;
  }

  /**
   * Send one command and return mgmtd's decision. Connection failures become an
   * `{ok:false,error}` reply so the dashboard can show them next to the command;
   * they are not errors in the relay itself.
   */
  async command(store: Mode, op: OperatorOp, args: CommandArgs): Promise<CommandReply> {
    const invalid = validateCommand(op, args);
    if (invalid) return { ok: false, error: invalid };
    const target = this.targets.find((t) => t.store === store);
    if (!target) return { ok: false, error: `no management endpoint for ${store}` };

    return this.serial(store, async () => {
      for (let attempt = 0; attempt < 2; attempt++) {
        let c = this.clients.get(store);
        try {
          if (!c) {
            c = await this.open(target);
            this.clients.set(store, c);
          }
          return (await c.command(op, wire(args))) as CommandReply;
        } catch (e) {
          this.clients.delete(store);
          c?.close();
          if (attempt === 1) return { ok: false, error: (e as Error).message };
        }
      }
      return { ok: false, error: "unreachable" };
    });
  }

  close() {
    for (const c of this.clients.values()) c.close();
    this.clients.clear();
  }
}
