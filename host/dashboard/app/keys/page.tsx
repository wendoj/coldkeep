"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Key requests: dangerous changes the governor queued (G5). The dashboard shows
// them but cannot approve them: approval happens at the store's local console, so
// a compromised network path can never approve its own request.
import { Waiting } from "@/components/Shell";
import { fmtSim, useRelay } from "@/lib/relay";

export default function KeysPage() {
  const r = useRelay();
  const s = r.stores.protected;
  if (!s) return <Waiting what="the COLDKEEP store" />;
  const reqs = s.keyRequests;
  return (
    <>
      <section className="panel">
        <div className="panel-head">
          <h1>Key requests</h1>
          <span className="sub">COLDKEEP store</span>
        </div>
        <p className="callout" style={{ margin: 0 }}>
          <b>Approve at the store&apos;s local console.</b> These requests can only be approved with the store key
          on site. This dashboard is on the network, so it has no approve button by design.
        </p>
      </section>
      <section className="panel">
        {reqs.length === 0 ? (
          <div className="empty">No pending key requests.</div>
        ) : (
          <div className="table-wrap">
            <table data-testid="key-requests">
              <thead><tr><th>Sim t</th><th>Requested by</th><th>Change</th><th>Status</th></tr></thead>
              <tbody>
                {reqs.map((d, i) => (
                  <tr key={i}>
                    <td className="num">{fmtSim(d.t)}</td>
                    <td>{d.user ?? "—"} via {d.origin ?? "mgmt"}</td>
                    <td className="mono">{d.cmd}{d.case ? ` case=${d.case}` : ""}</td>
                    <td>{d.reason} · <b>approve at the store&apos;s local console</b></td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
      {s.govStatus && (
        <section className="panel">
          <h2 style={{ marginBottom: 12 }}>Governor status <span className="muted" style={{ fontWeight: 400 }}>(/dev/coldkeep/status)</span></h2>
          <pre className="status-text">{s.govStatus}</pre>
        </section>
      )}
      <p className="muted" style={{ fontSize: 12 }}>Last update at sim {fmtSim(s.t)}.</p>
    </>
  );
}
