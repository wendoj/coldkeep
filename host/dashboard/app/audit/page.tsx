"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Audit: searchable decision log, the 15-minute compliance records with CSV
// export, the event log, and an incident report per store.
import { complianceCsv, type Mode } from "@coldkeep/shared";
import { Search } from "lucide-react";
import { useState } from "react";
import { Card, PageHeader, StoreToggle, VerdictBadge } from "@/components/kit";
import { Waiting } from "@/components/Shell";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { incidentReport, verdictText } from "@/lib/derive";
import { download, fmtSim, useRelay } from "@/lib/relay";
import { cn } from "@/lib/utils";

const th = "mono-label px-4 py-2 text-left font-normal whitespace-nowrap";
const td = "px-4 py-2.5 align-top";

export default function AuditPage() {
  const r = useRelay();
  const modes = Object.keys(r.stores) as Mode[];
  const [pick, setPick] = useState<Mode | null>(null);
  const [q, setQ] = useState("");
  const mode = pick && modes.includes(pick) ? pick : modes.includes("protected") ? "protected" : modes[0];
  const s = mode ? r.stores[mode] : undefined;
  if (!s) return <Waiting what="audit records" />;

  const needle = q.trim().toLowerCase();
  const decisions = s.decisions.filter((d) =>
    !needle || [d.cmd, d.verdict, verdictText(d), d.rule, d.reason, d.user, String(d.case ?? "")].join(" ").toLowerCase().includes(needle),
  );
  const label = s.meta?.label ?? "run";

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="Audit"
        actions={
          <>
            <StoreToggle stores={modes} value={s.mode} onChange={setPick} />
            <Button variant="outline" size="sm" onClick={() => download(`${label}.${s.mode}.compliance.csv`, complianceCsv(s), "text/csv")}>
              Compliance CSV
            </Button>
            <Button variant="outline" size="sm" onClick={() => download(`${label}.${s.mode}.incident.md`, incidentReport(s), "text/markdown")}>
              Incident report
            </Button>
          </>
        }
      />

      <Card bodyClassName="p-0">
        <Tabs defaultValue="decisions">
          <div className="flex flex-wrap items-center gap-3 border-b px-4 pt-2">
            <TabsList variant="line">
              <TabsTrigger value="decisions">Decisions <span className="num text-muted-foreground">{s.decisions.length}</span></TabsTrigger>
              <TabsTrigger value="compliance">Compliance <span className="num text-muted-foreground">{s.compliance.length}</span></TabsTrigger>
              <TabsTrigger value="events">Events <span className="num text-muted-foreground">{s.log.length}</span></TabsTrigger>
            </TabsList>
          </div>

          <TabsContent value="decisions">
            <div className="relative w-full border-b px-4 py-2 sm:w-80">
              <Search className="icon-regular pointer-events-none absolute top-1/2 left-6.5 size-3.5 -translate-y-1/2 text-muted-foreground" />
              <Input placeholder="Search decisions" value={q} onChange={(e) => setQ(e.target.value)} className="h-8 pl-8" aria-label="Search decisions" />
            </div>
            <div className="overflow-x-auto">
              <table className="w-full text-sm" data-testid="decision-log">
                <thead><tr className="border-b"><th className={th}>Time</th><th className={th}>User</th><th className={th}>Command</th><th className={th}>Verdict</th><th className={th}>Applied</th><th className={th}>Reason</th></tr></thead>
                <tbody>
                  {decisions.map((d, i) => (
                    <tr key={i} className="border-b last:border-0 hover:bg-muted">
                      <td className={cn(td, "num font-mono text-xs text-muted-foreground")}>{fmtSim(d.t)}</td>
                      <td className={td}>{d.user ?? "—"}</td>
                      <td className={cn(td, "font-mono text-xs")}>{d.cmd}{d.case ? ` case=${d.case}` : ""}{typeof d.value === "number" ? ` ${d.value}` : ""}</td>
                      <td className={td}><VerdictBadge d={d} /></td>
                      <td className={cn(td, "num")}>{typeof d.applied === "number" ? d.applied : "—"}</td>
                      <td className={cn(td, "text-muted-foreground")}>{d.reason}</td>
                    </tr>
                  ))}
                  {!decisions.length && <tr><td colSpan={6} className="p-4 text-sm text-muted-foreground">No decisions{needle ? " match" : " yet"}.</td></tr>}
                </tbody>
              </table>
            </div>
          </TabsContent>

          <TabsContent value="compliance">
            {s.compliance.length === 0 ? (
              <p className="p-4 text-sm text-muted-foreground">No compliance records in this feed.</p>
            ) : (
              <>
                <div className="overflow-x-auto">
                  <table className="w-full text-sm">
                    <thead><tr className="border-b"><th className={th}>Time</th>{s.compliance[0]!.cases.map((c) => <th key={c.id} className={th}>Case {c.id}</th>)}</tr></thead>
                    <tbody>
                      {s.compliance.map((rec, i) => (
                        <tr key={i} className="border-b last:border-0">
                          <td className={cn(td, "num font-mono text-xs text-muted-foreground")}>{fmtSim(rec.t)}</td>
                          {rec.cases.map((c) => {
                            const limit = s.cases[c.id - 1]?.info.limit;
                            const over = limit !== undefined && c.prod > limit;
                            return <td key={c.id} className={cn(td, "num", over && "font-semibold text-critical-ink")}>{c.prod.toFixed(1)}{over ? " ▲" : ""}</td>;
                          })}
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </>
            )}
          </TabsContent>

          <TabsContent value="events">
            {s.log.length === 0 ? (
              <p className="p-4 text-sm text-muted-foreground">No events.</p>
            ) : (
              <ul className="divide-y px-4 text-sm">
                {s.log.map((l, i) => (
                  <li key={i} className="flex gap-4 py-2.5">
                    <span className="num w-16 shrink-0 font-mono text-xs leading-5 text-muted-foreground">{fmtSim(l.t)}</span>
                    <span className="mono-label w-14 shrink-0 leading-5">{l.kind}</span>
                    <span>{l.text}</span>
                  </li>
                ))}
              </ul>
            )}
          </TabsContent>
        </Tabs>
      </Card>
    </div>
  );
}
