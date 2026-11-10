"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Commands: every attacker command paired with both stores' verdicts, and the
// privileged actions tried from inside the web process (A3).
import { computeReadouts, type Decision, type Mode, type Readouts, type StoreModel } from "@coldkeep/shared";
import { ChevronLeft, ChevronRight, Search } from "lucide-react";
import { useState } from "react";
import { Card, PageHeader, ResultBadge, StoreName, VerdictBadge } from "@/components/kit";
import { Waiting } from "@/components/Shell";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Tabs, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { pairDecisions, verdictText, type PairedRow } from "@/lib/derive";
import { fmtSim, storeLabel, useRelay } from "@/lib/relay";
import { cn } from "@/lib/utils";

const ORDER: Mode[] = ["legacy", "protected"];

/* ---------- commands ---------- */

const CMD: Record<string, string> = {
  start_defrost: "Start defrost",
  stop_defrost: "Stop defrost",
  set_setpoint: "Set setpoint",
  disable_alarms: "Disable alarms",
  set_envelope: "Change envelope",
  set_fans: "Set fans",
  raw_write: "Raw write",
  status: "Status",
};

function cmdDetail(row: PairedRow): string {
  const bits: string[] = [];
  if (row.caseId) bits.push(`case ${row.caseId}`);
  if (typeof row.value === "number") {
    if (row.cmd === "start_defrost") bits.push(row.value >= 3600 ? `${(row.value / 3600).toFixed(0)} h` : `${(row.value / 60).toFixed(0)} min`);
    else if (row.cmd === "set_setpoint") bits.push(`${row.value} °C`);
    else bits.push(String(row.value));
  }
  return bits.join(" · ");
}

type Bucket = "applied" | "clamped" | "denied" | "key";
const bucket = (d: Decision | undefined): Bucket | null => {
  if (!d) return null;
  const v = verdictText(d);
  return v === "APPLIED" || v === "ALLOW" ? "applied" : v === "CLAMP" ? "clamped" : v === "NEEDS_KEY" ? "key" : v === "DENY" ? "denied" : null;
};
const BUCKETS: { id: Bucket; label: string }[] = [
  { id: "applied", label: "Applied" },
  { id: "clamped", label: "Clamped" },
  { id: "denied", label: "Denied" },
  { id: "key", label: "Needs key" },
];

const th = "mono-label px-4 py-2 text-left font-normal whitespace-nowrap";
const td = "px-4 py-2.5";

function Totals({ stores, rs }: { stores: StoreModel[]; rs: Record<Mode, Readouts> }) {
  return (
    <div className="grid grid-cols-1 gap-px overflow-hidden rounded-md border bg-border sm:grid-cols-2">
      {stores.map((s) => (
        <div key={s.mode} className="flex items-center gap-6 bg-card px-4 py-3">
          <span className="text-sm"><StoreName mode={s.mode} /></span>
          <span className="ml-auto mono-label">Applied <span className="num ml-1 text-sm text-foreground" data-testid={`${s.mode}-applied`}>{rs[s.mode].applied}</span></span>
          <span className="mono-label">Blocked <span className="num ml-1 text-sm text-foreground" data-testid={`${s.mode}-blocked`}>{rs[s.mode].blocked}</span></span>
        </div>
      ))}
    </div>
  );
}

function Commands({ stores }: { stores: StoreModel[] }) {
  const [tab, setTab] = useState<"all" | Bucket>("all");
  const [q, setQ] = useState("");
  const [size, setSize] = useState(10);
  const [page, setPage] = useState(0);
  const rows = pairDecisions(stores);
  const ck = (row: PairedRow) => bucket(row.rows.protected);
  const needle = q.trim().toLowerCase();
  const shown = rows.filter((row) =>
    (tab === "all" || ck(row) === tab) &&
    (!needle || [row.cmd, CMD[row.cmd], cmdDetail(row), ...Object.values(row.rows).flatMap((d) => [d!.rule, d!.reason, verdictText(d!)])]
      .join(" ").toLowerCase().includes(needle)),
  );
  const pages = Math.max(1, Math.ceil(shown.length / size));
  const p = Math.min(page, pages - 1);
  const count = (b: Bucket) => rows.filter((row) => ck(row) === b).length;

  return (
    <Card
      title="Command log"
      actions={<span className="mono-label"><span className="num text-foreground" data-testid="command-count">{rows.length}</span> commands</span>}
      bodyClassName="p-0"
    >
      <div className="flex flex-wrap items-center gap-3 border-b px-4 py-2">
        <Tabs value={tab} onValueChange={(v) => { setTab(v as typeof tab); setPage(0); }}>
          <TabsList variant="line" className="h-8">
            <TabsTrigger value="all">All</TabsTrigger>
            {BUCKETS.filter((b) => b.id !== "applied" || count("applied") > 0).map((b) => (
              <TabsTrigger key={b.id} value={b.id} disabled={!count(b.id)}>
                {b.label} <span className="num text-muted-foreground">{count(b.id)}</span>
              </TabsTrigger>
            ))}
          </TabsList>
        </Tabs>
        <div className="relative ml-auto w-full sm:w-52">
          <Search className="icon-regular pointer-events-none absolute top-1/2 left-2.5 size-3.5 -translate-y-1/2 text-muted-foreground" />
          <Input placeholder="Search" value={q} onChange={(e) => { setQ(e.target.value); setPage(0); }} className="h-8 pl-8" aria-label="Search commands" />
        </div>
      </div>

      <div className="overflow-x-auto">
        <table className="w-full text-sm" data-testid="attack-table">
          <thead>
            <tr className="border-b">
              <th className={th}>Time</th>
              <th className={th}>Command</th>
              {stores.map((s) => <th key={s.mode} className={th}>{storeLabel(s.mode)}</th>)}
            </tr>
          </thead>
          <tbody>
            {shown.slice(p * size, p * size + size).map((row) => {
              const t = Math.min(...Object.values(row.rows).map((d) => d!.t));
              return (
                <tr key={row.key} className="border-b last:border-0 hover:bg-muted">
                  <td className={cn(td, "num font-mono text-xs text-muted-foreground")}>{fmtSim(t)}</td>
                  <td className={td}>
                    {CMD[row.cmd] ?? row.cmd}
                    <span className="ml-2 text-xs text-muted-foreground">{cmdDetail(row)}</span>
                  </td>
                  {stores.map((s) => <td key={s.mode} className={td}><VerdictBadge d={row.rows[s.mode]} /></td>)}
                </tr>
              );
            })}
            {!shown.length && (
              <tr><td colSpan={2 + stores.length} className="py-10 text-center text-muted-foreground">{rows.length ? "No commands match." : "No commands yet."}</td></tr>
            )}
          </tbody>
        </table>
      </div>

      <div className="flex items-center gap-2 border-t px-4 py-2 mono-label">
        Rows
        <Select value={String(size)} onValueChange={(v) => { setSize(Number(v)); setPage(0); }}>
          <SelectTrigger size="sm" className="w-16" aria-label="Rows per page"><SelectValue /></SelectTrigger>
          <SelectContent>{[10, 25, 50].map((n) => <SelectItem key={n} value={String(n)}>{n}</SelectItem>)}</SelectContent>
        </Select>
        <div className="ml-auto flex items-center gap-1">
          <Button variant="ghost" size="icon-sm" aria-label="Previous page" disabled={p === 0} onClick={() => setPage(p - 1)}><ChevronLeft /></Button>
          <span className="num px-1 font-mono">{p + 1} / {pages}</span>
          <Button variant="ghost" size="icon-sm" aria-label="Next page" disabled={p >= pages - 1} onClick={() => setPage(p + 1)}><ChevronRight /></Button>
        </div>
      </div>
    </Card>
  );
}

/* ---------- privileged attempts (A3) ---------- */

function Privileged({ stores }: { stores: StoreModel[] }) {
  const attacks = stores.flatMap((s) => s.attacks.map((a) => ({ store: s.mode, a })));
  const actions = [...new Set(attacks.map((x) => `${x.a.campaign}\u0000${x.a.action}`))];
  if (!actions.length) return null;
  return (
    <Card
      title="Privileged attempts"
      bodyClassName="p-0"
    >
      <div className="overflow-x-auto">
        <table className="w-full table-fixed text-sm" data-testid="a3-table">
          <thead>
            <tr className="border-b">
              <th className={cn(th, "w-[40%]")}>Attempt</th>
              {stores.map((s) => <th key={s.mode} className={th}>{storeLabel(s.mode)}</th>)}
            </tr>
          </thead>
          <tbody>
            {actions.map((k) => {
              const [campaign, action] = k.split("\u0000");
              return (
                <tr key={k} className="border-b last:border-0">
                  <td className={td}>
                    <span>{(action ?? "").replaceAll("_", " ").replace(/^./, (c) => c.toUpperCase())}</span>
                  </td>
                  {stores.map((s) => (
                    <td key={s.mode} className={td}>
                      <ResultBadge result={attacks.find((x) => x.store === s.mode && x.a.campaign === campaign && x.a.action === action)?.a.result} />
                    </td>
                  ))}
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </Card>
  );
}

export default function CommandsPage() {
  const r = useRelay();
  const stores = ORDER.map((m) => r.stores[m]).filter((s) => s !== undefined);
  if (stores.length < 2 || !stores.some((s) => s.history.length)) return <Waiting what="both stores" />;
  const rs = Object.fromEntries(stores.map((s) => [s.mode, computeReadouts(s)])) as Record<Mode, Readouts>;
  return (
    <div className="flex flex-col gap-6">
      <PageHeader title="Commands" />
      <Totals stores={stores} rs={rs} />
      <Commands stores={stores} />
      <Privileged stores={stores} />
    </div>
  );
}
