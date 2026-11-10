"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Control: send individual operator commands to the live QNX targets and watch
// mgmtd's decision come back. This is the same documented API the attacker uses;
// the difference is that the governor decides, not the web process.
//
// There is deliberately no "approve" control: dangerous changes (G5) are queued
// for the store's local keyconsole. Replay and simulator runs are read-only.
import type { CommandArgs, Decision, Mode, OperatorOp } from "@coldkeep/shared";
import { Loader2, Send } from "lucide-react";
import Link from "next/link";
import { useState } from "react";
import { Card, PageHeader, Pill, StoreName, VerdictBadge } from "@/components/kit";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group";
import { useCommands, type PendingCommand } from "@/lib/commands";
import { storeLabel, useRelay, type CommandResult } from "@/lib/relay";
import { cn } from "@/lib/utils";

type Target = "both" | Mode;
const BOTH: Mode[] = ["legacy", "protected"];

/** Operator-facing commands. Envelope/schedule/raw writes stay with the attacker API. */
const OPS: { id: OperatorOp; label: string; what: string }[] = [
  { id: "set_setpoint", label: "Set setpoint", what: "Ask the controller to run colder or warmer." },
  { id: "start_defrost", label: "Start defrost", what: "A manual defrost, with a duration." },
  { id: "stop_defrost", label: "Stop defrost", what: "Always safe: the governor allows it." },
  { id: "set_fans", label: "Set fans", what: "Fans off cuts cooling; G5 treats it as dangerous." },
  { id: "disable_alarms", label: "Disable alarms", what: "A G5 change: queued for the store's keyconsole." },
];

const PRESETS: { id: string; label: string; what: string; op: OperatorOp; args: (sp: number) => CommandArgs }[] = [
  { id: "plus1", label: "Setpoint +1 °C", what: "Inside the band: both stores apply it.", op: "set_setpoint", args: (sp) => ({ case: 1, value: sp + 1 }) },
  { id: "plus25", label: "Setpoint +25 °C", what: "Legacy applies; Coldkeep denies (G1).", op: "set_setpoint", args: (sp) => ({ case: 1, value: sp + 25 }) },
  { id: "defrost24h", label: "24 h defrost", what: "Legacy runs it; Coldkeep clamps to 45 min (G3).", op: "start_defrost", args: () => ({ case: 1, seconds: 86400 }) },
  { id: "alarmsOff", label: "Alarms off", what: "Legacy applies; Coldkeep queues a key request (G5).", op: "disable_alarms", args: () => ({}) },
];

/** Map a reply onto the decision shape the shared badge already understands. */
function asDecision(res: CommandResult): Decision | undefined {
  if (!res.reply.verdict) return undefined;
  return {
    type: "decision",
    t: res.at / 1000,
    cmd: res.reply.cmd ?? res.op,
    case: res.reply.case ?? res.args.case,
    value: res.args.value ?? res.args.seconds ?? null,
    verdict: res.reply.verdict,
    rule: res.reply.rule ?? "-",
    applied: res.reply.applied ?? null,
    reason: res.reply.reason,
  };
}

function Reply({ res }: { res: CommandResult }) {
  const d = asDecision(res);
  if (d) {
    return (
      <span className="inline-flex flex-wrap items-center gap-2">
        <VerdictBadge d={d} />
        {res.reply.verdict === "NEEDS_KEY" && (
          <Link href="/keys" className="text-xs text-muted-foreground underline underline-offset-2 hover:text-foreground">
            queued at the store console
          </Link>
        )}
      </span>
    );
  }
  if (res.reply.ok) return <Pill tone="good">OK</Pill>;
  return (
    <span className="inline-flex flex-wrap items-center gap-2">
      <Pill tone="bad">Error</Pill>
      <span className="text-xs text-muted-foreground">{res.reply.error ?? "no reply"}</span>
    </span>
  );
}

function ResultCell({ store, pending, result }: { store: Mode; pending?: PendingCommand; result?: CommandResult }) {
  return (
    <div className="flex min-w-0 flex-col gap-2 rounded-md border bg-card p-3" data-testid={`control-result-${store}`}>
      <StoreName mode={store} />
      {pending ? (
        <span className="inline-flex items-center gap-2 text-sm text-muted-foreground">
          <Loader2 className="size-3.5 motion-safe:animate-spin" /> sending
        </span>
      ) : result ? (
        <Reply res={result} />
      ) : (
        <span className="text-sm text-muted-foreground">No command yet.</span>
      )}
    </div>
  );
}

export default function ControlPage() {
  const r = useRelay();
  const { pending, results, send, can } = useCommands();
  const [target, setTarget] = useState<Target>("both");
  const [op, setOp] = useState<OperatorOp>("set_setpoint");
  const [caseId, setCaseId] = useState(1);
  const [value, setValue] = useState("4");
  const [seconds, setSeconds] = useState("3600");
  const [fansOn, setFansOn] = useState(true);

  const cases = (r.stores.protected ?? r.stores.legacy)?.cases.filter(Boolean) ?? [];
  const live = r.source === "live";
  const ready = live && can.length > 0;
  const targets: Mode[] = target === "both" ? BOTH.filter((m) => can.includes(m)) : [target];
  const latest = (m: Mode) => results.filter((x) => x.store === m).at(-1);
  const inflight = (m: Mode) => pending.find((x) => x.store === m);

  const args = (): CommandArgs => {
    switch (op) {
      case "set_setpoint":
        return { case: caseId, value: Number(value) };
      case "start_defrost":
        return { case: caseId, seconds: Math.round(Number(seconds)) };
      case "stop_defrost":
        return { case: caseId };
      case "set_fans":
        return { case: caseId, on: fansOn };
      case "disable_alarms":
        return {};
      default:
        return { case: caseId, value: Number(value) };
    }
  };

  const sendTo = (stores: Mode[], a: CommandArgs, o: OperatorOp = op) => {
    for (const s of stores) send(s, o, a);
  };

  const go = () => sendTo(targets, args());

  const preset = (p: (typeof PRESETS)[number]) => {
    // Per-store setpoint, so "+25" is 25 degrees above that store's own value.
    for (const m of BOTH) {
      if (!can.includes(m)) continue;
      const sp = r.stores[m]?.cases.find((c) => c?.info.id === 1)?.latest?.sp ?? 3;
      send(m, p.op, p.args(sp));
    }
  };

  const recent = [...results].reverse().slice(0, 12);

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="Control"
        actions={
          <span className="mono-label">
            {ready ? "Live · QNX targets" : live ? "Relay has no control channel" : "Read-only"}
          </span>
        }
      />

      {!ready && (
        <Card title="This view needs the live relay" data-testid="control-offline">
          <div className="flex flex-col gap-3 text-sm">
            <p className="text-muted-foreground">
              {live
                ? "The relay is connected to the targets but was started without management endpoints, so it cannot send commands."
                : "Replay and simulator runs are read-only: the dashboard never pretends to command a recording."}
            </p>
            <pre className="overflow-x-auto rounded-md bg-muted/60 p-3 font-mono text-xs leading-relaxed">{`cd host
npm run relay -- --store legacy=127.0.0.1:17071 --store protected=127.0.0.1:17070 \\
  --mgmt legacy=127.0.0.1:18081 --mgmt protected=127.0.0.1:18080`}</pre>
            <p className="text-xs text-muted-foreground">
              Both QEMU stores must be running and <code className="font-mono">scripts/deploy.sh 0|1</code> must have started{" "}
              <code className="font-mono">mgmtd</code>.
            </p>
          </div>
        </Card>
      )}

      <div className="grid grid-cols-1 gap-6 xl:grid-cols-3">
        <Card title="Send a command" className="xl:col-span-2" bodyClassName="flex flex-col gap-4" data-testid="control-form">
          <div className="flex flex-wrap items-end gap-3">
            <div className="flex flex-col gap-1.5">
              <Label>Send to</Label>
              <ToggleGroup type="single" variant="outline" size="sm" value={target} onValueChange={(v) => v && setTarget(v as Target)} aria-label="Target">
                <ToggleGroupItem value="both" className="px-3" disabled={!ready}>Both</ToggleGroupItem>
                <ToggleGroupItem value="legacy" className="px-3" disabled={!ready || !can.includes("legacy")}>Legacy</ToggleGroupItem>
                <ToggleGroupItem value="protected" className="px-3" disabled={!ready || !can.includes("protected")}>Coldkeep</ToggleGroupItem>
              </ToggleGroup>
            </div>

            <div className="flex min-w-44 flex-1 flex-col gap-1.5">
              <Label>Command</Label>
              <Select value={op} onValueChange={(v) => setOp(v as OperatorOp)} disabled={!ready}>
                <SelectTrigger aria-label="Command"><SelectValue /></SelectTrigger>
                <SelectContent>{OPS.map((o) => <SelectItem key={o.id} value={o.id}>{o.label}</SelectItem>)}</SelectContent>
              </Select>
            </div>

            {op !== "disable_alarms" && (
              <div className="flex w-28 flex-col gap-1.5">
                <Label>Case</Label>
                <Select value={String(caseId)} onValueChange={(v) => setCaseId(Number(v))} disabled={!ready}>
                  <SelectTrigger aria-label="Case"><SelectValue /></SelectTrigger>
                  <SelectContent>
                    {(cases.length ? cases : Array.from({ length: 8 }, (_, i) => ({ info: { id: i + 1 } }))).map((c) => (
                      <SelectItem key={c.info.id} value={String(c.info.id)}>
                        {c.info.id}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
            )}

            {op === "set_setpoint" && (
              <div className="flex w-28 flex-col gap-1.5">
                <Label htmlFor="value">°C</Label>
                <Input id="value" inputMode="decimal" value={value} onChange={(e) => setValue(e.target.value)} disabled={!ready} />
              </div>
            )}

            {op === "start_defrost" && (
              <div className="flex w-36 flex-col gap-1.5">
                <Label htmlFor="seconds">Seconds</Label>
                <Input id="seconds" inputMode="numeric" value={seconds} onChange={(e) => setSeconds(e.target.value)} disabled={!ready} />
              </div>
            )}

            {op === "set_fans" && (
              <div className="flex w-32 flex-col gap-1.5">
                <Label>Fans</Label>
                <Select value={fansOn ? "on" : "off"} onValueChange={(v) => setFansOn(v === "on")} disabled={!ready}>
                  <SelectTrigger aria-label="Fans"><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="on">On</SelectItem><SelectItem value="off">Off</SelectItem></SelectContent>
                </Select>
              </div>
            )}

            <Button onClick={go} disabled={!ready || !targets.length} data-testid="control-send">
              <Send /> Send
            </Button>
          </div>

          <p className="text-xs text-muted-foreground">{OPS.find((o) => o.id === op)?.what}</p>

          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            {targets.map((m) => <ResultCell key={m} store={m} pending={inflight(m)} result={latest(m)} />)}
          </div>

          <p className="text-xs text-muted-foreground">
            Current setpoints:{" "}
            {BOTH.map((m) => {
              const sp = r.stores[m]?.cases.find((c) => c?.info.id === caseId)?.latest?.sp;
              return `${storeLabel(m)} ${sp === undefined ? "—" : `${sp.toFixed(1)} °C`}`;
            }).join(" · ")}
            {op === "disable_alarms" && " · applies to every case"}
          </p>
        </Card>

        <Card title="Try this" bodyClassName="flex flex-col gap-2">
          {PRESETS.map((p) => (
            <button
              key={p.id}
              onClick={() => preset(p)}
              disabled={!ready}
              data-testid={`control-preset-${p.id}`}
              className={cn(
                "flex flex-col gap-0.5 rounded-md border p-3 text-left text-sm transition-colors",
                "hover:bg-muted/60 disabled:cursor-not-allowed disabled:opacity-50",
              )}
            >
              <span className="font-medium">{p.label}</span>
              <span className="text-xs text-muted-foreground">{p.what}</span>
            </button>
          ))}
          <p className="pt-1 text-xs text-muted-foreground">
            Each preset goes to both stores so the same command lands in one audit log twice, with two verdicts.
          </p>
        </Card>
      </div>

      <Card title="Recent commands" actions={<span className="mono-label">{results.length} replies</span>} bodyClassName="p-0">
        {recent.length === 0 ? (
          <p className="p-4 text-sm text-muted-foreground">Nothing sent yet. Use a preset or the form above.</p>
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-sm" data-testid="control-recent">
              <thead>
                <tr className="border-b">
                  <th className="mono-label px-4 py-2 text-left font-normal">Time</th>
                  <th className="mono-label px-4 py-2 text-left font-normal">Store</th>
                  <th className="mono-label px-4 py-2 text-left font-normal">Command</th>
                  <th className="mono-label px-4 py-2 text-left font-normal">Case</th>
                  <th className="mono-label px-4 py-2 text-left font-normal">Value</th>
                  <th className="mono-label px-4 py-2 text-left font-normal">Result</th>
                </tr>
              </thead>
              <tbody>
                {recent.map((x) => (
                  <tr key={x.id} className="border-b last:border-0">
                    <td className="num px-4 py-2.5 font-mono text-xs text-muted-foreground">
                      {new Date(x.at).toLocaleTimeString()}
                    </td>
                    <td className="px-4 py-2.5"><StoreName mode={x.store} /></td>
                    <td className="px-4 py-2.5">{OPS.find((o) => o.id === x.op)?.label ?? x.op}</td>
                    <td className="num px-4 py-2.5 font-mono text-xs">{x.args.case ?? "—"}</td>
                    <td className="num px-4 py-2.5 font-mono text-xs">
                      {x.args.value ?? x.args.seconds ?? (x.args.on === undefined ? "—" : x.args.on ? "on" : "off")}
                    </td>
                    <td className="px-4 py-2.5"><Reply res={x} /></td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Card>

      <p className="max-w-3xl text-xs leading-relaxed text-muted-foreground">
        Every command above is an ordinary authenticated request to <code className="font-mono">mgmtd</code> on the target, using the
        documented API and the default <code className="font-mono">admin/admin</code> login. The dashboard holds no special path into
        the plant: on the Coldkeep store the request still crosses <code className="font-mono">brokerd</code> and is decided by{" "}
        <code className="font-mono">governord</code>. Key requests are never approved here. Approval is a local action at{" "}
        <code className="font-mono">keyconsole</code>, so a compromised web path cannot approve its own request. The same page in replay
        or simulator mode is read-only.
      </p>
    </div>
  );
}
