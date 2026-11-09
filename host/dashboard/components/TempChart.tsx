"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Product temperature over sim time, one line per store, with the food-safe limit.
// Crosshair + tooltip on hover/focus; a table view for every chart.
import { useEffect, useMemo, useRef, useState } from "react";
import type { Mode } from "@coldkeep/shared";
import { fmtSim, storeColor, storeLabel } from "@/lib/relay";

export interface Series {
  store: Mode;
  points: { t: number; v: number }[];
}

const H = 240, L = 44, R = 92, T = 14, B = 30;

function ticks(lo: number, hi: number, n: number): number[] {
  const span = hi - lo || 1;
  const raw = span / n;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 5, 10].map((k) => k * mag).find((s) => span / s <= n) ?? mag * 10;
  const out: number[] = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(Math.round(v * 1000) / 1000);
  return out;
}

export function TempChart({ title, series, limit, testId }: { title: string; series: Series[]; limit: number; testId?: string }) {
  const [hover, setHover] = useState<number | null>(null);
  const [table, setTable] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  // draw at the container's real width so text stays at its intended size
  const [W, setW] = useState(640);
  const hasData = series.some((x) => x.points.length > 0);
  useEffect(() => {
    const el = box.current;
    if (!el) return;
    const ro = new ResizeObserver(([e]) => e && setW(Math.max(320, Math.round(e.contentRect.width))));
    ro.observe(el);
    return () => ro.disconnect();
  }, [table, hasData]);

  const g = useMemo(() => {
    const all = series.flatMap((s) => s.points);
    if (!all.length) return null;
    const t0 = Math.min(...all.map((p) => p.t));
    const t1 = Math.max(...all.map((p) => p.t), t0 + 60);
    const vs = all.map((p) => p.v).concat(limit);
    let lo = Math.min(...vs), hi = Math.max(...vs);
    const pad = Math.max(1, (hi - lo) * 0.1);
    lo -= pad; hi += pad;
    const x = (t: number) => L + ((t - t0) / (t1 - t0)) * (W - L - R);
    const y = (v: number) => T + (1 - (v - lo) / (hi - lo)) * (H - T - B);
    const times = [...new Set(all.map((p) => p.t))].sort((a, b) => a - b);
    return { t0, t1, lo, hi, x, y, times };
  }, [series, limit, W]);

  if (!g) return <div className="panel empty">No samples yet for {title.toLowerCase()}.</div>;

  const valueAt = (s: Series, t: number) => {
    let best: { t: number; v: number } | null = null;
    for (const p of s.points) if (!best || Math.abs(p.t - t) < Math.abs(best.t - t)) best = p;
    return best;
  };
  const onMove = (clientX: number) => {
    const el = box.current;
    if (!el) return;
    const rect = el.getBoundingClientRect();
    const sx = ((clientX - rect.left) / rect.width) * W;
    const t = g.t0 + ((sx - L) / (W - L - R)) * (g.t1 - g.t0);
    let best = g.times[0]!;
    for (const tt of g.times) if (Math.abs(tt - t) < Math.abs(best - t)) best = tt;
    setHover(best);
  };
  const xTicks = ticks(g.t0 / 60, g.t1 / 60, 6);
  const yTicks = ticks(g.lo, g.hi, 5);

  return (
    <div className="panel" data-testid={testId}>
      <div className="panel-head">
        <h2>{title}</h2>
        <div className="legend" aria-label="Legend">
          {series.map((s) => (
            <span key={s.store}>
              <span className="store-key" style={{ background: storeColor(s.store) }} />
              {storeLabel(s.store)}
            </span>
          ))}
          <span>
            <span className="store-key" style={{ background: "var(--limit)", height: 0, borderTop: "2px dashed var(--limit)" }} />
            limit {limit} °C
          </span>
        </div>
        <button className="btn" style={{ marginLeft: "auto" }} onClick={() => setTable(!table)} aria-pressed={table}>
          {table ? "Chart" : "Table"}
        </button>
      </div>
      {table ? (
        <div className="table-wrap" style={{ maxHeight: 260, overflowY: "auto" }}>
          <table>
            <thead>
              <tr><th>Sim time</th>{series.map((s) => <th key={s.store}>{storeLabel(s.store)} °C</th>)}</tr>
            </thead>
            <tbody>
              {g.times.map((t) => (
                <tr key={t}>
                  <td className="num">{fmtSim(t)}</td>
                  {series.map((s) => {
                    const p = s.points.find((q) => q.t === t);
                    return <td key={s.store} className="num">{p ? p.v.toFixed(2) : "—"}</td>;
                  })}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : (
        <div
          className="chart"
          ref={box}
          tabIndex={0}
          aria-label={`${title}: product temperature over sim time. Use the table view for values.`}
          onPointerMove={(e) => onMove(e.clientX)}
          onPointerLeave={() => setHover(null)}
          onBlur={() => setHover(null)}
          onKeyDown={(e) => {
            if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
            const i = hover === null ? g.times.length - 1 : g.times.indexOf(hover) + (e.key === "ArrowRight" ? 1 : -1);
            setHover(g.times[Math.max(0, Math.min(g.times.length - 1, i))]!);
          }}
        >
          <svg viewBox={`0 0 ${W} ${H}`} role="img" aria-hidden="true">
            {yTicks.map((v) => (
              <g key={v}>
                <line x1={L} x2={W - R} y1={g.y(v)} y2={g.y(v)} stroke="var(--grid)" strokeWidth={1} />
                <text x={L - 6} y={g.y(v) + 4} textAnchor="end" fontSize={11} fill="var(--muted)" className="num">{v}</text>
              </g>
            ))}
            <line x1={L} x2={W - R} y1={H - B} y2={H - B} stroke="var(--axis)" />
            {xTicks.map((m) => (
              <text key={m} x={g.x(m * 60)} y={H - B + 16} textAnchor="middle" fontSize={11} fill="var(--muted)" className="num">
                {m} min
              </text>
            ))}
            <line x1={L} x2={W - R} y1={g.y(limit)} y2={g.y(limit)} stroke="var(--limit)" strokeWidth={1.5} strokeDasharray="5 4" />
            <text x={L + 4} y={g.y(limit) - 5} fontSize={11} fill="var(--limit)">limit {limit} °C</text>
            {series.map((s) => {
              if (!s.points.length) return null;
              const d = s.points.map((p, i) => `${i ? "L" : "M"}${g.x(p.t).toFixed(1)},${g.y(p.v).toFixed(1)}`).join("");
              const last = s.points[s.points.length - 1]!;
              return (
                <g key={s.store}>
                  <path d={d} fill="none" stroke={storeColor(s.store)} strokeWidth={2} strokeLinejoin="round" strokeLinecap="round" />
                  <text x={g.x(last.t) + 8} y={g.y(last.v) + 4} fontSize={12} fill="var(--ink-2)" fontWeight={600}>
                    {s.store === "legacy" ? "Legacy" : "COLDKEEP"}
                  </text>
                </g>
              );
            })}
            {hover !== null && (
              <g>
                <line x1={g.x(hover)} x2={g.x(hover)} y1={T} y2={H - B} stroke="var(--ink-2)" strokeWidth={1} />
                {series.map((s) => {
                  const p = valueAt(s, hover);
                  return p ? (
                    <circle key={s.store} cx={g.x(p.t)} cy={g.y(p.v)} r={4.5} fill={storeColor(s.store)} stroke="var(--surface)" strokeWidth={2} />
                  ) : null;
                })}
              </g>
            )}
          </svg>
          {hover !== null && (
            <div
              className="tooltip"
              style={{
                left: `${(g.x(hover) / W) * 100}%`,
                top: 8,
                transform: g.x(hover) > W / 2 ? "translateX(calc(-100% - 12px))" : "translateX(12px)",
              }}
            >
              <div className="t num">sim {fmtSim(hover)}</div>
              {series.map((s) => {
                const p = valueAt(s, hover);
                return (
                  <div className="r" key={s.store}>
                    <span className="store-key" style={{ background: storeColor(s.store), marginRight: 0 }} />
                    <b className="num">{p ? `${p.v.toFixed(1)} °C` : "—"}</b>
                    <span className="muted">{storeLabel(s.store)}</span>
                  </div>
                );
              })}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
