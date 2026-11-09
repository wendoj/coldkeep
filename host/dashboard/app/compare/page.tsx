"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Compare view: the same seed and attack on both stores, side by side. Readouts
// come from computeReadouts() over the stores' events (the run's summary line
// once it has arrived), never from constants.
import { computeReadouts, type Mode, type Readouts } from "@coldkeep/shared";
import { Waiting } from "@/components/Shell";
import { TempChart } from "@/components/TempChart";
import { worstSeries } from "@/lib/derive";
import { storeColor, storeLabel, useRelay } from "@/lib/relay";

const ORDER: Mode[] = ["legacy", "protected"];

function ReadoutCard({ mode, r }: { mode: Mode; r: Readouts }) {
  const items: [string, string, string, string][] = [
    ["minutes", "Case-minutes above limit", r.minutesAbove.toFixed(1), "min"],
    ["peak", "Peak chilled product", r.peakChilled === null ? "—" : r.peakChilled.toFixed(1), "°C"],
    ["applied", "Attacker commands applied", String(r.applied), `of ${r.applied + r.blocked}`],
    ["blocked", "Attacker commands blocked", String(r.blocked), `of ${r.applied + r.blocked}`],
    ["food", "Illustrative food at risk", r.foodAtRiskKg.toLocaleString("en-GB"), "kg"],
  ];
  if (r.a3) items.push(["a3", "Privileged actions succeeded", String(r.a3.succeeded), `of ${r.a3.attempts}`]);
  return (
    <section className="panel" data-testid={`readouts-${mode}`} data-source={r.source}>
      <div className="panel-head">
        <h2>
          <span className="store-key" style={{ background: storeColor(mode), height: 4, width: 20 }} />
          {storeLabel(mode)}
        </h2>
        <span className="source-note">
          {r.source === "summary" ? "from the run's recorded summary" : "running, computed from samples so far"}
        </span>
      </div>
      <div className="readouts">
        {items.map(([k, label, v, unit]) => (
          <div className="readout" key={k}>
            <div className="label">{label}</div>
            <div className="value num" data-testid={`${mode}-${k}`}>
              {v}
              <small>{unit}</small>
            </div>
          </div>
        ))}
      </div>
    </section>
  );
}

export default function ComparePage() {
  const r = useRelay();
  const stores = ORDER.map((m) => r.stores[m]).filter((s) => s !== undefined);
  if (!stores.length || !stores.some((s) => s.history.length)) return <Waiting what="two stores' telemetry" />;
  const meta = stores[0]!.meta;

  return (
    <>
      <section className="panel">
        <div className="panel-head">
          <h1>Same attack, two stores</h1>
          <span className="sub">
            {r.source === "replay" ? "Replay of a recorded run" : "Live from the QNX targets"}
            {meta?.label ? ` · ${meta.label}` : ""}
            {meta?.seed !== undefined ? ` · seed ${meta.seed}` : ""}
            {meta?.source ? ` · ${meta.source}` : ""}
            {meta?.git ? ` · commit ${meta.git}` : ""}
          </span>
        </div>
        <p className="callout" style={{ margin: 0 }}>
          Both stores run the same plant, the same seed and the same attacker. The attacker logs in with the default{" "}
          <code>admin/admin</code> and orders 24-hour defrosts, setpoints 25 °C higher and alarms off. The legacy store
          applies everything. COLDKEEP&apos;s governor clamps, denies or queues each command for the local store key.
        </p>
      </section>

      <div className="grid-2">
        {stores.map((s) => (
          <ReadoutCard key={s.mode} mode={s.mode} r={computeReadouts(s)} />
        ))}
      </div>

      <TempChart testId="chart-chilled" title="Chilled cases: warmest product" limit={8} series={stores.map((s) => worstSeries(s, "chilled"))} />
      <div className="grid-2">
        <TempChart title="Frozen wells: warmest product" limit={-18} series={stores.map((s) => worstSeries(s, "frozen"))} />
        <TempChart title="Cold rooms: warmest product" limit={5} series={stores.map((s) => worstSeries(s, "coldroom"))} />
      </div>
    </>
  );
}
