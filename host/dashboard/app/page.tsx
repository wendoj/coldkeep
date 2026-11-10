"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Overview: did the food stay safe under the same attack? Every number is derived
// from the stores' events (computeReadouts), never constants. Detail lives on the
// other pages.
import { computeReadouts, type Mode, type Readouts, type StoreModel } from "@coldkeep/shared";
import { Card, Eyebrow, PageHeader, StoreDot } from "@/components/kit";
import { Waiting } from "@/components/Shell";
import { TempChart } from "@/components/TempChart";
import { RULE_NAME, ruleCounts } from "@/lib/derive";
import { storeLabel, useRelay } from "@/lib/relay";

const ORDER: Mode[] = ["protected", "legacy"];

function Kpi({ label, k, values, fmt }: { label: string; k: string; values: Record<Mode, number | null>; fmt: (v: number) => string }) {
  return (
    <div className="flex flex-col gap-4 bg-card p-4">
      <Eyebrow>{label}</Eyebrow>
      <dl className="flex flex-col gap-2">
        {ORDER.map((m) => (
          <div key={m} className="flex items-baseline justify-between gap-3">
            <dt className="inline-flex items-center gap-2 text-xs text-muted-foreground">
              <StoreDot mode={m} />
              {storeLabel(m)}
            </dt>
            <dd className="num font-mono text-xl tracking-tight" data-testid={`${m}-${k}`}>
              {values[m] === null ? "—" : fmt(values[m]!)}
            </dd>
          </div>
        ))}
      </dl>
    </div>
  );
}

function TopRules({ s }: { s: StoreModel | undefined }) {
  const top = (s ? ruleCounts(s) : []).slice(0, 5);
  return (
    <Card title="Rules that fired" bodyClassName="p-0">
      {top.length === 0 ? (
        <p className="p-4 text-sm text-muted-foreground">None yet.</p>
      ) : (
        <table className="w-full text-sm" data-testid="top-rules">
          <thead>
            <tr className="border-b">
              <th className="mono-label px-4 py-2 text-left font-normal">Rule</th>
              <th className="mono-label px-4 py-2 text-right font-normal">Count</th>
            </tr>
          </thead>
          <tbody>
            {top.map((c) => (
              <tr key={c.rule} className="border-b last:border-0">
                <td className="px-4 py-2.5">{RULE_NAME[c.rule] ?? c.rule}</td>
                <td className="num px-4 py-2.5 text-right font-mono">{c.n}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </Card>
  );
}

export default function Overview() {
  const r = useRelay();
  const stores = ORDER.map((m) => r.stores[m]).filter((s) => s !== undefined);
  if (stores.length < 2 || !stores.some((s) => s.history.length)) return <Waiting what="both stores" />;
  const rs = Object.fromEntries(stores.map((s) => [s.mode, computeReadouts(s)])) as Record<Mode, Readouts>;
  const v = (f: (x: Readouts) => number | null) => ({ legacy: f(rs.legacy), protected: f(rs.protected) });

  return (
    <div className="flex flex-col gap-6">
      <PageHeader title="Overview" />

      <div
        className="grid grid-cols-1 gap-px overflow-hidden rounded-md border bg-border sm:grid-cols-2 xl:grid-cols-4"
        data-testid="outcome"
        data-source-legacy={rs.legacy.source}
        data-source-protected={rs.protected.source}
      >
        <Kpi label="Minutes above limit" k="minutes" values={v((x) => x.minutesAbove)} fmt={(x) => x.toFixed(1)} />
        <Kpi label="Peak chilled °C" k="peak" values={v((x) => x.peakChilled)} fmt={(x) => x.toFixed(1)} />
        <Kpi label="Food at risk kg (est.)" k="food" values={v((x) => x.foodAtRiskKg)} fmt={(x) => x.toLocaleString("en-GB")} />
        <Kpi label="Commands applied" k="applied" values={v((x) => x.applied)} fmt={String} />
      </div>

      <div className="grid grid-cols-1 gap-6 xl:grid-cols-3">
        <TempChart className="xl:col-span-2" />
        <TopRules s={r.stores.protected} />
      </div>
    </div>
  );
}
