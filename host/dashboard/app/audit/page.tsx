"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Audit and compliance: searchable decision log, the 15-minute compliance records
// with CSV export, and an incident report per store.
import { useState } from "react";
import { complianceCsv, type Mode } from "@coldkeep/shared";
import { Waiting } from "@/components/Shell";
import { StorePicker } from "@/components/StorePicker";
import { incidentReport, verdictText } from "@/lib/derive";
import { download, fmtSim, useRelay } from "@/lib/relay";

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
    !needle || [d.cmd, d.verdict, d.rule, d.reason, d.user, String(d.case ?? "")].join(" ").toLowerCase().includes(needle),
  );
  const label = s.meta?.label ?? "live";

  return (
    <>
      <section className="panel">
        <div className="panel-head">
          <h1>Audit & compliance</h1>
          <StorePicker stores={modes} value={s.mode} onChange={setPick} />
          <div className="row" style={{ marginLeft: "auto" }}>
            <button className="btn" onClick={() => download(`${label}.${s.mode}.compliance.csv`, complianceCsv(s), "text/csv")}>
              Export compliance CSV
            </button>
            <button className="btn" onClick={() => download(`${label}.${s.mode}.incident.md`, incidentReport(s), "text/markdown")}>
              Download incident report
            </button>
          </div>
        </div>
        <div className="row">
          <input type="search" placeholder="Search command, verdict, rule, reason…" value={q} onChange={(e) => setQ(e.target.value)} aria-label="Search decisions" />
          <span className="muted">{decisions.length} of {s.decisions.length} decisions</span>
        </div>
      </section>

      <section className="panel">
        <h2 style={{ marginBottom: 12 }}>Decision log</h2>
        {decisions.length === 0 ? (
          <div className="empty">No decisions{needle ? " match" : " yet"}.</div>
        ) : (
          <div className="table-wrap">
            <table data-testid="decision-log">
              <thead>
                <tr><th>Sim t</th><th>User</th><th>Command</th><th>Verdict</th><th>Rule</th><th>Applied</th><th>Reason</th></tr>
              </thead>
              <tbody>
                {decisions.map((d, i) => (
                  <tr key={i}>
                    <td className="num">{fmtSim(d.t)}</td>
                    <td>{d.user ?? "—"}</td>
                    <td className="mono">{d.cmd}{d.case ? ` case=${d.case}` : ""}{typeof d.value === "number" ? ` ${d.value}` : ""}</td>
                    <td><span className="verdict" data-v={verdictText(d) === "APPLIED" ? "applied" : d.verdict}>{verdictText(d)}</span></td>
                    <td className="mono">{d.rule}</td>
                    <td className="num">{typeof d.applied === "number" ? d.applied : "—"}</td>
                    <td className="reason">{d.reason}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>

      <div className="grid-2">
        <section className="panel">
          <h2 style={{ marginBottom: 12 }}>Compliance records (every 15 sim-min)</h2>
          {s.compliance.length === 0 ? (
            <div className="empty">No compliance records in this feed.</div>
          ) : (
            <div className="table-wrap">
              <table>
                <thead>
                  <tr><th>Sim t</th>{s.compliance[0]!.cases.map((c) => <th key={c.id}>Case {c.id}</th>)}</tr>
                </thead>
                <tbody>
                  {s.compliance.map((rec, i) => (
                    <tr key={i}>
                      <td className="num">{fmtSim(rec.t)}</td>
                      {rec.cases.map((c) => {
                        const limit = s.cases[c.id - 1]?.info.limit;
                        const over = limit !== undefined && c.prod > limit;
                        return (
                          <td key={c.id} className="num" style={over ? { color: "var(--critical-ink)", fontWeight: 700 } : undefined}>
                            {c.prod.toFixed(1)}{over ? " ▲" : ""}
                          </td>
                        );
                      })}
                    </tr>
                  ))}
                </tbody>
              </table>
              <p className="muted" style={{ fontSize: 12 }}>Product °C; ▲ marks a reading above the case&apos;s limit.</p>
            </div>
          )}
        </section>
        <section className="panel">
          <h2 style={{ marginBottom: 12 }}>Events</h2>
          {s.log.length === 0 ? (
            <div className="empty">No events.</div>
          ) : (
            <ul style={{ margin: 0, paddingLeft: 18, fontSize: 13 }}>
              {s.log.map((l, i) => (
                <li key={i}><span className="num muted">{fmtSim(l.t)}</span> <b>{l.kind}</b> {l.text}</li>
              ))}
            </ul>
          )}
        </section>
      </div>
    </>
  );
}
