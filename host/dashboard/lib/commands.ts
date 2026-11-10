"use client";
// SPDX-License-Identifier: Apache-2.0
//
// One operator command in flight. The relay echoes our id on command_result, so
// the UI can show "sending" until the target's mgmtd has decided, then keep the
// reply next to the command that caused it.
import { useEffect, useState } from "react";
import type { CommandArgs, Mode, OperatorOp } from "@coldkeep/shared";
import { useRelay } from "@/lib/relay";

export interface PendingCommand {
  id: string;
  store: Mode;
  op: OperatorOp;
  args: CommandArgs;
  at: number;
}

// The relay's operator client retries once (connect 10 s, reply 30 s per attempt),
// so a real reply can take up to ~80 s on a sick target. Wait longer than that:
// clearing the row earlier would make a command that is still in flight look
// like it never happened. A closed relay never echoes, hence the bound.
const TIMEOUT_MS = 90_000;

export function useCommands() {
  const r = useRelay();
  const [pending, setPending] = useState<PendingCommand[]>([]);
  const results = r.commandResults;

  useEffect(() => {
    if (!pending.length) return;
    const done = new Set(results.map((x) => x.id));
    const left = pending.filter((x) => !done.has(x.id));
    if (left.length !== pending.length) setPending(left);
  }, [results, pending]);

  const send = (store: Mode, op: OperatorOp, args: CommandArgs = {}) => {
    const id = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
    setPending((p) => [...p, { id, store, op, args, at: Date.now() }]);
    r.send({ kind: "command", id, store, op, args });
    // A closed relay never echoes: drop the spinner rather than lie about it.
    setTimeout(() => setPending((p) => p.filter((x) => x.id !== id)), TIMEOUT_MS);
    return id;
  };

  return { pending, results, send, can: r.can.control ?? [] };
}
