"use client";
// SPDX-License-Identifier: Apache-2.0
import { Menu, Pause, Play, RotateCcw } from "lucide-react";
import { SwapIcon } from "@/components/kit";
import Link from "next/link";
import { usePathname } from "next/navigation";
import { useState, type ReactNode } from "react";
import { RATES, RunPanelProvider, useRunPanel } from "@/components/RunPanel";
import { Button } from "@/components/ui/button";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import { fmtSim, useRelay, type RelayState } from "@/lib/relay";
import { cn } from "@/lib/utils";

const NAV = [
  ["/", "Overview"],
  ["/cases", "Cases"],
  ["/commands", "Commands"],
  ["/control", "Control"],
  ["/audit", "Audit"],
  ["/keys", "Key requests"],
  ["/system", "System"],
] as const;

/**
 * Where the numbers come from. Kept visible so host-simulator results are never
 * mistaken for runs on the QNX targets.
 */
function sourceLabel(r: RelayState): string {
  if (!r.connected) return "Relay offline";
  const origin = Object.values(r.stores).find((s) => s?.meta?.source)?.meta?.source ?? "";
  if (r.source === "live") return "Live · QNX targets";
  if (r.source === "sim" || origin === "sim-host") return "Host simulator";
  return /qemu/.test(origin) ? "Recorded on QEMU" : `Recording${origin ? ` · ${origin}` : ""}`;
}

function StatusDot({ state }: { state: "on" | "off" | "paused" }) {
  return (
    <span
      aria-hidden
      className={cn(
        "inline-block size-1.5 shrink-0 rounded-full",
        state === "on" && "bg-coldkeep",
        state === "paused" && "bg-warn",
        state === "off" && "bg-foreground/40",
      )}
    />
  );
}

function NavLinks({ path, onNavigate }: { path: string; onNavigate?: () => void }) {
  return (
    <nav aria-label="Views" className="flex flex-col gap-px">
      {NAV.map(([href, text]) => {
        const on = path === href;
        return (
          <Link
            key={href}
            href={href}
            onClick={onNavigate}
            aria-current={on ? "page" : undefined}
            className={cn(
              "rounded-md px-2.5 py-1.5 text-sm transition-colors",
              on ? "bg-accent text-foreground" : "text-muted-foreground hover:bg-accent/60 hover:text-foreground",
            )}
          >
            {text}
          </Link>
        );
      })}
    </nav>
  );
}

function RunButton() {
  const { open, available } = useRunPanel();
  const r = useRelay();
  const btn = (
    <Button onClick={open} disabled={!available} className="w-full">
      Run attack
    </Button>
  );
  if (available) return btn;
  return (
    <Tooltip>
      <TooltipTrigger asChild><span tabIndex={0} className="block">{btn}</span></TooltipTrigger>
      <TooltipContent>
        {r.connected ? "This relay has no way to run attacks (start it with no arguments, or with --mgmt)." : "Relay offline"}
      </TooltipContent>
    </Tooltip>
  );
}

function Playback() {
  const r = useRelay();
  if (!r.can.playback || !r.playback) return null;
  const p = r.playback;
  const rateValue = String(p.rate);
  return (
    <div className="flex items-center gap-1" aria-label="Playback">
      <Button
        variant="ghost"
        size="icon-xs"
        aria-label={p.paused ? "Resume" : "Pause"}
        disabled={p.done}
        onClick={() => r.send({ kind: "playback", action: p.paused ? "resume" : "pause" })}
      >
        <SwapIcon on={p.paused} active={<Play className="translate-x-px" />} inactive={<Pause />} />
      </Button>
      <Select value={RATES.some((x) => x.value === rateValue) ? rateValue : undefined} onValueChange={(v) => r.send({ kind: "rate", rate: RATES.find((x) => x.value === v)!.rate })}>
        <SelectTrigger size="sm" className="h-6 gap-1 border-0 bg-transparent px-1.5 font-mono text-[11px] tracking-[0.04rem] uppercase shadow-none" aria-label="Speed">
          <SelectValue placeholder={`${rateValue}×`} />
        </SelectTrigger>
        <SelectContent>
          {RATES.map((x) => <SelectItem key={x.value} value={x.value}>{x.label}</SelectItem>)}
        </SelectContent>
      </Select>
      <Button variant="ghost" size="icon-xs" aria-label="Restart" onClick={() => r.send({ kind: "playback", action: "restart" })}>
        <RotateCcw />
      </Button>
    </div>
  );
}

function Brand() {
  return (
    <Link href="/" className="font-mono text-sm tracking-[0.08rem] uppercase">
      Coldkeep
    </Link>
  );
}

function Status() {
  const r = useRelay();
  const state = !r.connected ? "off" : r.playback?.paused ? "paused" : "on";
  return (
    <span className="inline-flex items-center gap-2 font-mono text-[11px] tracking-[0.04rem] uppercase" data-testid="connection" title={r.url}>
      <StatusDot state={state} />
      {sourceLabel(r)}
    </span>
  );
}

function SidebarBody({ path, onNavigate }: { path: string; onNavigate?: () => void }) {
  return (
    <div className="flex h-full flex-col gap-6 px-3 py-4">
      <div className="px-2.5"><Brand /></div>
      <div className="flex flex-col gap-2">
        <div className="mono-label px-2.5">Workspace</div>
        <NavLinks path={path} onNavigate={onNavigate} />
      </div>
      <div className="mt-auto flex flex-col gap-3 border-t pt-4">
        <div className="px-1"><Status /></div>
        <RunButton />
      </div>
    </div>
  );
}

function TopBar({ path, onMenu }: { path: string; onMenu: () => void }) {
  const r = useRelay();
  const t = Math.max(0, ...Object.values(r.stores).map((s) => s?.t ?? 0));
  const current = NAV.find(([href]) => href === path)?.[1] ?? "Overview";
  return (
    <header className="sticky top-0 z-20 flex h-12 items-center gap-3 border-b bg-background/95 px-4 backdrop-blur sm:px-6">
      <Button variant="ghost" size="icon-sm" className="lg:hidden" aria-label="Menu" onClick={onMenu}>
        <Menu />
      </Button>
      <nav aria-label="Breadcrumb" className="flex items-center gap-2 font-mono text-[11px] tracking-[0.04rem] uppercase">
        <span className="text-faint">Coldkeep</span>
        <span className="text-faint">/</span>
        <span className="text-foreground">{current}</span>
      </nav>
      <div className="ml-auto flex items-center gap-3">
        {Object.keys(r.stores).length > 0 && (
          <span className="mono-label">
            Sim <span className="num text-foreground" data-testid="sim-time">{fmtSim(t)}</span>
          </span>
        )}
        <Playback />
      </div>
    </header>
  );
}

export function Shell({ children }: { children: ReactNode }) {
  const path = usePathname();
  const [menu, setMenu] = useState(false);
  return (
    <RunPanelProvider>
      <div className="min-h-screen bg-background lg:pl-56">
        <aside className="fixed inset-y-0 left-0 z-30 hidden w-56 border-r bg-muted lg:block">
          <SidebarBody path={path} />
        </aside>
        {menu && (
          <div className="fixed inset-0 z-40 lg:hidden">
            <button className="absolute inset-0 bg-foreground/10" aria-label="Close menu" onClick={() => setMenu(false)} />
            <aside className="absolute inset-y-0 left-0 w-56 border-r bg-muted">
              <SidebarBody path={path} onNavigate={() => setMenu(false)} />
            </aside>
          </div>
        )}
        <TopBar path={path} onMenu={() => setMenu(true)} />
        <main className="mx-auto w-full max-w-[1200px] px-4 py-6 sm:px-6 sm:py-8">{children}</main>
      </div>
    </RunPanelProvider>
  );
}

export function Waiting({ what = "data" }: { what?: string }) {
  const r = useRelay();
  const { open, available } = useRunPanel();
  return (
    <div className="flex flex-col items-start gap-4 rounded-md border px-6 py-16">
      <div className="mono-label">{r.connected ? "Waiting" : "Offline"}</div>
      <div className="text-sm">
        {r.connected ? `No data from ${what} yet.` : <>Relay offline. Start it with <code className="font-mono">npm run relay</code> in <code className="font-mono">host/</code>.</>}
      </div>
      {available && <Button onClick={open}>Run attack</Button>}
    </div>
  );
}
