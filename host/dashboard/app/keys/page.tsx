"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Key requests: dangerous changes the governor queued (G5). The dashboard shows
// them but cannot approve them: approval happens at the store's local console, so
// a compromised network path can never approve its own request.
import { Card, PageHeader, Pill } from "@/components/kit";
import { Waiting } from "@/components/Shell";
import { fmtSim, useRelay } from "@/lib/relay";

const th = "mono-label px-4 py-2 text-left font-normal whitespace-nowrap";
const td = "px-4 py-2.5";

export default function KeysPage() {
  const r = useRelay();
  const s = r.stores.protected;
  if (!s) return <Waiting what="the Coldkeep store" />;
  const reqs = s.keyRequests;
  const pending = reqs.filter((d) => s.keyState(d) === "pending").length;

  return (
    <div className="flex flex-col gap-6">
      <PageHeader title="Key requests" actions={<span className="mono-label">{pending} pending</span>} />
      <p className="text-sm text-muted-foreground">Approved only at the store&apos;s local console.</p>

      <Card title="Queue" actions={<span className="mono-label">{reqs.length} total</span>} bodyClassName="p-0">
        {reqs.length === 0 ? (
          <p className="p-4 text-sm text-muted-foreground">No key requests.</p>
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-sm" data-testid="key-requests">
              <thead><tr className="border-b"><th className={th}>Time</th><th className={th}>Requested by</th><th className={th}>Change</th><th className={th}>Status</th></tr></thead>
              <tbody>
                {reqs.map((d, i) => {
                  const st = s.keyState(d);
                  return (
                    <tr key={i} className="border-b align-top last:border-0">
                      <td className={`${td} num font-mono text-xs text-muted-foreground`}>{fmtSim(d.t)}</td>
                      <td className={td}>{d.user ?? "—"} <span className="text-muted-foreground">via {d.origin ?? "mgmt"}</span></td>
                      <td className={`${td} font-mono text-xs`}>{d.cmd}{d.case ? ` case=${d.case}` : ""}</td>
                      <td className={td} data-testid="key-state">
                        <Pill tone={st === "pending" ? "key" : st === "approved" ? "good" : "neutral"}>{st}</Pill>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </Card>

      {s.govStatus && (
        <Card title="Governor status" bodyClassName="p-0">
          <pre className="overflow-x-auto p-4 font-mono text-xs leading-5">{s.govStatus}</pre>
        </Card>
      )}
    </div>
  );
}
