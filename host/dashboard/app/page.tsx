"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Store view: one store's floor, each case coloured by product temperature with an
// icon, a label and the number (never colour alone).
import { useState } from "react";
import type { Mode } from "@coldkeep/shared";
import { StatusBadge } from "@/components/Status";
import { StorePicker } from "@/components/StorePicker";
import { Waiting } from "@/components/Shell";
import { caseStatus } from "@/lib/derive";
import { fmtSim, storeLabel, useRelay } from "@/lib/relay";

const TYPE_LABEL = { chilled: "Chilled multideck", frozen: "Frozen well", coldroom: "Walk-in cold room" } as const;

export default function StorePage() {
  const r = useRelay();
  const modes = Object.keys(r.stores) as Mode[];
  const [pick, setPick] = useState<Mode | null>(null);
  const mode = pick && modes.includes(pick) ? pick : modes.includes("protected") ? "protected" : modes[0];
  const s = mode ? r.stores[mode] : undefined;
  if (!s || !s.cases.some((c) => c?.latest)) return <Waiting what="telemetry" />;

  const cases = s.cases.filter(Boolean);
  const statuses = cases.map(caseStatus);
  const inRange = statuses.filter((x) => x === "ok").length;
  const over = statuses.filter((x) => x === "over").length;
  const warn = statuses.filter((x) => x === "warn").length;

  return (
    <>
      <section className="panel">
        <div className="panel-head">
          <h1>{storeLabel(s.mode)} store</h1>
          <StorePicker stores={modes} value={s.mode} onChange={setPick} />
        </div>
        <div className="stats">
          <div className="stat"><div className="label">Mode</div><div className="value">{s.mode}</div></div>
          <div className="stat"><div className="label">Sim time</div><div className="value num" data-testid="sim-time">{fmtSim(s.t)}</div></div>
          <div className="stat"><div className="label">Cases in range</div><div className="value num">{inRange} / {cases.length}</div></div>
          <div className="stat"><div className="label">Over limit</div><div className="value num">{over}</div></div>
          <div className="stat"><div className="label">Warnings</div><div className="value num">{warn}</div></div>
          <div className="stat">
            <div className="label">Source</div>
            <div className="value" style={{ fontSize: 15, paddingTop: 6 }}>{s.meta?.source ?? "?"}{s.meta?.speed ? ` · ${s.meta.speed}× sim speed` : ""}</div>
          </div>
        </div>
      </section>

      <section className="floor" aria-label="Cases">
        {cases.map((c, i) => {
          const st = statuses[i]!;
          const l = c.latest;
          const prod = l ? l.prod_true ?? l.prod : null;
          return (
            <article key={c.info.id} className="tile" data-status={st} data-testid={`case-${c.info.id}`}>
              <div className="head">
                <span>Case {c.info.id}</span>
                <span>{TYPE_LABEL[c.info.type]}</span>
              </div>
              <div className="temp num">
                {prod === null ? "—" : prod.toFixed(1)}
                <small>°C product</small>
              </div>
              <StatusBadge status={st} />
              {l && (
                <>
                  <div className="meta num">
                    <span>air {(l.air_true ?? l.air).toFixed(1)} °C</span>
                    <span>setpoint {l.sp.toFixed(1)}</span>
                    <span>limit {c.info.limit}</span>
                  </div>
                  <div className="chips">
                    <span className="chip" data-on={!!l.comp}>compressor {l.comp ? "on" : "off"}</span>
                    <span className="chip" data-on={!!l.defrost}>defrost {l.defrost ? "on" : "off"}</span>
                    <span className="chip" data-on={!!l.door}>door {l.door ? "open" : "shut"}</span>
                    {!!l.hold && <span className="chip" data-on="true">G6 hold</span>}
                  </div>
                  {l.jitter_us_max !== undefined && (
                    <div className="meta num muted">loop jitter (max, last s) {(l.jitter_us_max / 1000).toFixed(1)} ms</div>
                  )}
                </>
              )}
            </article>
          );
        })}
      </section>
    </>
  );
}
