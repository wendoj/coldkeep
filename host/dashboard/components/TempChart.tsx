"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Warmest product temperature over sim time, one line per store, with the
// food-safe limit. The case type is the dashboard's one filter.
import type { CaseType, Mode } from "@coldkeep/shared";
import { LIMIT_C } from "@coldkeep/shared";
import { useMemo, useState } from "react";
import { CartesianGrid, Line, LineChart, ReferenceLine, XAxis, YAxis } from "recharts";
import { Card, StoreDot } from "@/components/kit";
import { ChartContainer, ChartTooltip, ChartTooltipContent, type ChartConfig } from "@/components/ui/chart";
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group";
import { mergeSeries, worstSeries } from "@/lib/derive";
import { fmtSim, storeLabel, useRelay } from "@/lib/relay";
import { cn } from "@/lib/utils";

const config = {
  protected: { label: "Coldkeep", color: "var(--coldkeep)" },
  legacy: { label: "Legacy", color: "var(--legacy)" },
} satisfies ChartConfig;

const TYPES: { id: CaseType; label: string }[] = [
  { id: "chilled", label: "Chilled" },
  { id: "frozen", label: "Frozen" },
  { id: "coldroom", label: "Cold room" },
];

const tick = { fontFamily: "var(--font-mono)", fontSize: 11, fill: "var(--faint)" };

export function TempChart({ className }: { className?: string }) {
  const r = useRelay();
  const [type, setType] = useState<CaseType>("chilled");
  const stores = (["protected", "legacy"] as const).map((m) => r.stores[m]).filter((s) => s !== undefined);
  const limit = stores[0]?.cases.find((c) => c?.info.type === type)?.info.limit ?? LIMIT_C[type];
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const data = useMemo(() => mergeSeries(stores.map((s) => worstSeries(s, type, 360))), [r.rev, type]);

  return (
    <Card
      className={cn("min-w-0", className)}
      data-testid={`chart-${type}`}
      title="Warmest product °C"
      actions={
        <ToggleGroup type="single" variant="outline" size="sm" value={type} onValueChange={(v) => v && setType(v as CaseType)} aria-label="Case type">
          {TYPES.map((t) => (
            <ToggleGroupItem key={t.id} value={t.id} className="px-2.5 font-mono text-[11px] tracking-[0.04rem] uppercase data-[state=on]:bg-accent">
              {t.label}
            </ToggleGroupItem>
          ))}
        </ToggleGroup>
      }
    >
      {!data.length ? (
        <div className="grid h-72 place-items-center text-sm text-muted-foreground">No samples yet.</div>
      ) : (
        <ChartContainer config={config} className="aspect-auto h-72 w-full">
          <LineChart data={data} margin={{ left: 0, right: 8, top: 8, bottom: 0 }}>
            <CartesianGrid vertical={false} stroke="var(--border)" />
            <XAxis
              dataKey="t"
              type="number"
              domain={["dataMin", "dataMax"]}
              tickLine={false}
              axisLine={false}
              tickMargin={8}
              tick={tick}
              tickFormatter={(t: number) => `${Math.round(t / 60)}m`}
            />
            <YAxis
              width={32}
              tickLine={false}
              axisLine={false}
              tick={tick}
              domain={[(lo: number) => Math.floor(Math.min(lo, limit) - 1), (hi: number) => Math.ceil(Math.max(hi, limit) + 1)]}
            />
            <ReferenceLine y={limit} stroke="var(--foreground)" strokeOpacity={0.5} strokeDasharray="3 3" strokeWidth={1} />
            <ChartTooltip
              cursor={{ stroke: "var(--input)" }}
              content={
                <ChartTooltipContent
                  indicator="line"
                  labelFormatter={(_, p) => fmtSim(Number(p?.[0]?.payload?.t ?? 0))}
                  formatter={(v, name) => (
                    <div className="flex w-full items-center justify-between gap-4">
                      <span className="text-muted-foreground">{config[name as Mode]?.label}</span>
                      <span className="num font-mono">{Number(v).toFixed(1)}</span>
                    </div>
                  )}
                />
              }
            />
            {stores.map((s) => (
              <Line
                key={s.mode}
                dataKey={s.mode}
                type="monotone"
                stroke={`var(--color-${s.mode})`}
                strokeWidth={s.mode === "protected" ? 2 : 1.5}
                dot={false}
                isAnimationActive={false}
                connectNulls
              />
            ))}
          </LineChart>
        </ChartContainer>
      )}

      <div className="mt-3 flex flex-wrap gap-x-5 gap-y-2 font-mono text-[11px] tracking-[0.04rem] text-muted-foreground uppercase">
        {stores.map((s) => (
          <span key={s.mode} className="inline-flex items-center gap-2"><StoreDot mode={s.mode} />{storeLabel(s.mode)}</span>
        ))}
        <span className="inline-flex items-center gap-2">
          <span className="inline-block w-3 border-t border-dashed border-foreground/50" />
          Limit {limit}
        </span>
      </div>
    </Card>
  );
}
