"use client";
// SPDX-License-Identifier: Apache-2.0
//
// System: the frontend/backend seam. The browser receives events over the relay
// WebSocket; this view also asks the dashboard's own `/api/relay` route for the
// relay's HTTP hello, so health and capabilities are visible in one place.
import type { Mode } from "@coldkeep/shared";
import { CircleCheck, CircleX } from "lucide-react";
import { useEffect, useState } from "react";
import { Card, PageHeader, StoreDot } from "@/components/kit";
import { Button } from "@/components/ui/button";
import { fetchRelayHealth, type RelayHealth } from "@/lib/relay-info";
import { fmtSim, storeLabel, useRelay } from "@/lib/relay";
import { cn } from "@/lib/utils";

const ORDER: Mode[] = ["protected", "legacy"];

function Status({ ok, on, off }: { ok: boolean; on: string; off: string }) {
  const Icon = ok ? CircleCheck : CircleX;
  return (
    <span className={cn("inline-flex items-center gap-1.5 text-xs", ok ? "text-good-ink" : "text-critical-ink")}>
      <Icon className="size-3.5" aria-hidden />
      {ok ? on : off}
    </span>
  );
}

function Row({ k, children }: { k: string; children: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-4 px-4 py-2.5">
      <dt className="mono-label">{k}</dt>
      <dd className="min-w-0 truncate text-right font-mono text-xs">{children}</dd>
    </div>
  );
}

function BackendCard({ health, refresh }: { health: RelayHealth | null; refresh: () => void }) {
  const hello = health?.hello;
  const can = hello?.can;
  return (
    <Card title="Relay" actions={<Button variant="outline" size="sm" onClick={refresh}>Refresh</Button>} bodyClassName="p-0">
      <dl className="divide-y">
        <Row k="Status">{health === null ? "…" : <Status ok={health.ok} on="Reachable" off="Unreachable" />}</Row>
        <Row k="URL"><span title={health?.url}>{health?.url || "—"}</span></Row>
        <Row k="Source">{hello?.source ?? "—"}</Row>
        <Row k="Stores">{hello?.stores.join(", ") || "—"}</Row>
        <Row k="Recordings">{hello?.files?.length ?? "—"}</Row>
        <Row k="Playback">{can ? (can.playback ? "on" : "off") : "—"}</Row>
        <Row k="Simulator">{can ? (can.sim ? "on" : "off") : "—"}</Row>
        <Row k="Live campaigns">{can ? (can.live.length ? can.live.join(", ") : "off") : "—"}</Row>
      </dl>
      {health?.error && <p className="border-t px-4 py-2.5 font-mono text-xs">{health.error}</p>}
    </Card>
  );
}

function StoreFeed() {
  const r = useRelay();
  const rows = ORDER.map((m) => r.stores[m]).filter((s) => s !== undefined);
  return (
    <Card title="Store feeds" bodyClassName="p-0">
      {rows.length === 0 ? (
        <p className="p-4 text-sm text-muted-foreground">No store feed. Start the relay or open a recording.</p>
      ) : (
        <ul className="divide-y">
          {rows.map((s) => {
            const connected = r.storeStatus[s.mode]?.connected;
            return (
              <li key={s.mode} className="flex flex-wrap items-center gap-x-4 gap-y-2 px-4 py-2.5">
                <span className="inline-flex min-w-28 items-center gap-2 text-sm font-medium">
                  <StoreDot mode={s.mode} />
                  {storeLabel(s.mode)}
                </span>
                <span className={cn("mono-label inline-flex items-center gap-1.5", connected && "text-good-ink")}>
                  <span className={cn("size-1.5 rounded-full", connected ? "bg-coldkeep" : "bg-muted-foreground/50")} />
                  {connected === undefined ? (r.source === "replay" ? "replay" : "waiting") : connected ? "connected" : "disconnected"}
                  {r.ended[s.mode] && " · ended"}
                </span>
                <span className="num ml-auto font-mono text-xs">{fmtSim(s.t)}</span>
              </li>
            );
          })}
        </ul>
      )}
    </Card>
  );
}

function ProcessLog() {
  const r = useRelay();
  const rows = ORDER.flatMap((m) => (r.stores[m]?.log ?? []).map((l) => ({ ...l, mode: m })))
    .filter((l) => l.kind === "proc" || l.kind === "note" || l.kind === "g6")
    .sort((a, b) => b.t - a.t)
    .slice(0, 20);
  return (
    <Card title="Process events" bodyClassName="p-0">
      {rows.length === 0 ? (
        <p className="p-4 text-sm text-muted-foreground">No process events in this feed.</p>
      ) : (
        <ul className="divide-y px-4 text-xs">
          {rows.map((l, i) => (
            <li key={`${l.mode}-${l.t}-${i}`} className="flex gap-3 py-2.5">
              <span className="num w-14 shrink-0 font-mono text-muted-foreground">{fmtSim(l.t)}</span>
              <span className="inline-flex w-16 shrink-0 items-center gap-1.5 font-mono text-[10.5px] text-muted-foreground">
                <StoreDot mode={l.mode} />
                {l.kind}
              </span>
              <span className="min-w-0 leading-5">{l.text}</span>
            </li>
          ))}
        </ul>
      )}
    </Card>
  );
}

function Governor() {
  const r = useRelay();
  const text = r.stores.protected?.govStatus;
  return (
    <Card title="Governor status" bodyClassName="p-0">
      {text ? (
        <pre className="max-h-72 overflow-auto p-4 font-mono text-[11px] leading-5">{text}</pre>
      ) : (
        <p className="p-4 text-sm text-muted-foreground">No governor status in this feed.</p>
      )}
    </Card>
  );
}

export default function SystemPage() {
  const [health, setHealth] = useState<RelayHealth | null>(null);
  const load = () => { void fetchRelayHealth().then(setHealth); };
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      const h = await fetchRelayHealth();
      if (alive) setHealth(h);
    };
    void tick();
    const id = setInterval(tick, 5000);
    return () => { alive = false; clearInterval(id); };
  }, []);

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="System"
      />
      <div className="grid grid-cols-1 gap-4 xl:grid-cols-2">
        <BackendCard health={health} refresh={load} />
        <StoreFeed />
        <ProcessLog />
        <Governor />
      </div>
    </div>
  );
}
