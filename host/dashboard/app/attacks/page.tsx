"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Attack console: every attacker command with each store's verdict and rule.
import type { Decision, Mode } from "@coldkeep/shared";
import { Waiting } from "@/components/Shell";
import { pairDecisions, verdictText } from "@/lib/derive";
import { storeColor, storeLabel, useRelay } from "@/lib/relay";

const ORDER: Mode[] = ["legacy", "protected"];

function Verdict({ d }: { d: Decision | undefined }) {
  if (!d) return <span className="muted">—</span>;
  const v = verdictText(d);
  return (
    <>
      <span className="verdict" data-v={v === "APPLIED" ? "applied" : d.verdict}>{v}</span>
      {d.rule !== "LEGACY" && <span className="mono"> {d.rule}</span>}
      {d.reason && d.rule !== "LEGACY" && <div className="reason">{d.reason}</div>}
    </>
  );
}

export default function AttacksPage() {
  const r = useRelay();
  const stores = ORDER.map((m) => r.stores[m]).filter((s) => s !== undefined);
  if (!stores.length) return <Waiting what="decisions" />;
  const rows = pairDecisions(stores);
  const attacks = stores.flatMap((s) => s.attacks.map((a) => ({ store: s.mode, a })));
  const actions = [...new Set(attacks.map((x) => `${x.a.campaign}\u0000${x.a.action}`))];

  return (
    <>
      <section className="panel">
        <div className="panel-head">
          <h1>Attack console</h1>
          <span className="sub">{rows.length} commands through the management API</span>
        </div>
        {rows.length === 0 ? (
          <div className="empty">No commands yet. Run <code>npm run attack -- --store legacy=… --store protected=…</code></div>
        ) : (
          <div className="table-wrap">
            <table data-testid="attack-table">
              <thead>
                <tr>
                  <th>Sim t</th>
                  <th>Command</th>
                  {stores.map((s) => (
                    <th key={s.mode}>
                      <span className="store-key" style={{ background: storeColor(s.mode) }} />
                      {storeLabel(s.mode)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.map((row) => {
                  const t = Math.min(...Object.values(row.rows).map((d) => d!.t));
                  return (
                    <tr key={row.key}>
                      <td className="num">{t.toFixed(0)} s</td>
                      <td className="mono">
                        {row.cmd}
                        {row.caseId ? ` case=${row.caseId}` : ""}
                        {typeof row.value === "number" ? ` value=${row.value}` : ""}
                      </td>
                      {stores.map((s) => (
                        <td key={s.mode}><Verdict d={row.rows[s.mode]} /></td>
                      ))}
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </section>

      <section className="panel">
        <div className="panel-head">
          <h2>Privileged actions from inside the web process (A3)</h2>
          <span className="sub">shell commands run with mgmtd&apos;s uid on the target; errors come from QNX</span>
        </div>
        {actions.length === 0 ? (
          <div className="empty">None in this run. A3 is run on the target by <code>scripts/run-compare.py</code>.</div>
        ) : (
          <div className="table-wrap">
            <table data-testid="a3-table">
              <thead>
                <tr><th>Attempt</th>{stores.map((s) => <th key={s.mode}>{storeLabel(s.mode)}</th>)}</tr>
              </thead>
              <tbody>
                {actions.map((k) => {
                  const [campaign, action] = k.split("\u0000");
                  return (
                    <tr key={k}>
                      <td>{campaign} {action}</td>
                      {stores.map((s) => {
                        const a = attacks.find((x) => x.store === s.mode && x.a.campaign === campaign && x.a.action === action)?.a;
                        const res = a?.result ?? "—";
                        return (
                          <td key={s.mode} className="mono">
                            <span className="verdict" data-v={res === "SUCCEEDED" ? "applied" : res.startsWith("n/a") ? "" : "DENY"}>
                              {res}
                            </span>
                          </td>
                        );
                      })}
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </>
  );
}
