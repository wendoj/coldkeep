"use client";
// SPDX-License-Identifier: Apache-2.0
//
// "Run attack": choose campaigns and send them to both stores through the relay
// (host simulator, or the live targets' management API). Approving key requests
// is deliberately not possible from here.
import type { Campaign, Rate } from "@coldkeep/shared";
import { Dices, Loader2, Play } from "lucide-react";
import { SwapIcon } from "@/components/kit";
import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { Button } from "@/components/ui/button";
import { Checkbox } from "@/components/ui/checkbox";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Sheet, SheetContent, SheetDescription, SheetFooter, SheetHeader, SheetTitle } from "@/components/ui/sheet";
import { useRelay } from "@/lib/relay";
import { cn } from "@/lib/utils";

export const CAMPAIGNS: { id: Campaign; name: string; what: string }[] = [
  { id: "A1", name: "Credentialed takeover", what: "Logs in as admin/admin, orders 24-hour defrosts, setpoints 25 °C higher and alarms off." },
  { id: "A2", name: "API flood", what: "Floods the management API with status requests." },
  { id: "A3", name: "Compromised web process", what: "Code inside the web process tries to reach the plant directly." },
  { id: "A4", name: "Governor crash", what: "Kills the safety governor mid-run." },
];

export const RATES: { value: string; rate: Rate; label: string }[] = [
  { value: "30", rate: 30, label: "30× (1 h in 2 min)" },
  { value: "60", rate: 60, label: "60× (1 h in 1 min)" },
  { value: "120", rate: 120, label: "120× (1 h in 30 s)" },
  { value: "max", rate: "max", label: "Instant" },
];

const Ctx = createContext<{ open: () => void; available: boolean }>({ open: () => {}, available: false });
export const useRunPanel = () => useContext(Ctx);

/** Colour the verdicts in one transcript line: "[A1] t=601 … -> legacy: APPLIED | coldkeep: CLAMP (G3 …)". */
function LogLine({ text }: { text: string }) {
  const parts = text.split(/(APPLIED|SUCCEEDED|CLAMP|DENY|NEEDS_KEY|EPERM|EACCES|served|rate-limited)/);
  const tone = (p: string) =>
    p === "APPLIED" || p === "SUCCEEDED" ? "text-critical-ink font-semibold"
    : ["CLAMP", "DENY", "EPERM", "EACCES"].includes(p) ? "text-good-ink font-semibold"
    : p === "NEEDS_KEY" ? "text-warn-ink font-semibold"
    : "";
  return (
    <div className="border-b border-border/60 py-1.5 last:border-0">
      {parts.map((p, i) => (
        <span key={i} className={tone(p)}>{p}</span>
      ))}
    </div>
  );
}

export function RunPanelProvider({ children }: { children: ReactNode }) {
  const r = useRelay();
  const [open, setOpen] = useState(false);
  const live = r.source === "live";
  const allowed: Campaign[] = live ? r.can.live : r.can.sim ? ["A1", "A2", "A3", "A4"] : [];
  const available = r.connected && allowed.length > 0;
  const [picked, setPicked] = useState<Campaign[]>(["A1", "A2", "A3", "A4"]);
  const [seed, setSeed] = useState("7");
  const [rate, setRate] = useState("60");
  const running = r.run?.state === "running";
  const chosen = picked.filter((c) => allowed.includes(c));
  const logEnd = useRef<HTMLDivElement>(null);
  useEffect(() => {
    logEnd.current?.scrollIntoView({ block: "end" });
  }, [r.logs.length, open]);

  const go = () => {
    const seedN = Math.max(0, Math.min(2 ** 31, Math.floor(Number(seed) || 0)));
    if (live) r.send({ kind: "run", target: "live", campaigns: chosen });
    else r.send({ kind: "run", target: "sim", campaigns: chosen, seed: seedN, rate: RATES.find((x) => x.value === rate)!.rate });
  };

  return (
    <Ctx.Provider value={{ open: () => setOpen(true), available }}>
      {children}
      <Sheet open={open} onOpenChange={setOpen}>
        <SheetContent className="flex w-full flex-col gap-0 sm:max-w-md">
          <SheetHeader className="border-b">
            <SheetTitle>Run an attack</SheetTitle>
            <SheetDescription>
              {live
                ? "Sent to both QNX targets' management API with the default admin login."
                : "Both stores run in the host simulator with the same plant, seed and attacker."}
            </SheetDescription>
          </SheetHeader>

          <div className="flex min-h-0 flex-1 flex-col gap-5 overflow-y-auto overscroll-contain p-4">
            <fieldset className="flex flex-col gap-2">
              <legend className="mono-label mb-2">Campaigns</legend>
              {CAMPAIGNS.map((c) => {
                const ok = allowed.includes(c.id);
                const on = picked.includes(c.id) && ok;
                return (
                  <Label
                    key={c.id}
                    className={cn(
                      "flex cursor-pointer items-start gap-3 rounded-lg border p-3 font-normal transition-colors hover:bg-muted/60",
                      on && "border-input bg-muted",
                      !ok && "cursor-not-allowed opacity-50",
                    )}
                  >
                    <Checkbox
                      checked={on}
                      disabled={!ok || running}
                      onCheckedChange={(v) => setPicked((p) => (v ? [...p, c.id] : p.filter((x) => x !== c.id)))}
                      className="mt-0.5"
                    />
                    <span className="flex flex-col gap-0.5">
                      <span className="text-sm font-medium">
                        <span className="mono-label mr-2">{c.id}</span>
                        {c.name}
                      </span>
                      <span className="text-xs leading-snug text-muted-foreground">
                        {ok ? c.what : "Run from the store's own shell, not over the network."}
                      </span>
                    </span>
                  </Label>
                );
              })}
            </fieldset>

            {!live && (
              <div className="grid grid-cols-2 gap-3">
                <div className="flex flex-col gap-1.5">
                  <Label htmlFor="seed">Seed</Label>
                  <div className="flex gap-1">
                    <Input id="seed" inputMode="numeric" value={seed} onChange={(e) => setSeed(e.target.value.replace(/\D/g, ""))} disabled={running} />
                    <Button
                      variant="outline"
                      size="icon"
                      aria-label="Random seed"
                      disabled={running}
                      onClick={() => setSeed(String(Math.floor(Math.random() * 100000)))}
                    >
                      <Dices />
                    </Button>
                  </div>
                </div>
                <div className="flex flex-col gap-1.5">
                  <Label>Speed</Label>
                  <Select value={rate} onValueChange={setRate} disabled={running}>
                    <SelectTrigger className="w-full" aria-label="Run speed"><SelectValue /></SelectTrigger>
                    <SelectContent>
                      {RATES.map((x) => <SelectItem key={x.value} value={x.value}>{x.label}</SelectItem>)}
                    </SelectContent>
                  </Select>
                </div>
              </div>
            )}

            {r.run?.state === "error" && (
              <p className="rounded-lg bg-critical/10 p-3 text-sm text-critical-ink">Run failed: {r.run.detail}</p>
            )}

            <div className="flex min-h-40 flex-1 flex-col">
              <div className="mono-label mb-2">Log</div>
              <div className="min-h-0 flex-1 overflow-y-auto overscroll-contain rounded-lg bg-muted/60 px-3 py-1 font-mono text-[11.5px] leading-relaxed" data-testid="attack-log">
                {r.logs.length ? r.logs.map((l, i) => <LogLine key={i} text={l} />) : <div className="py-3 text-muted-foreground">Nothing yet.</div>}
                <div ref={logEnd} />
              </div>
            </div>
          </div>

          <SheetFooter className="border-t">
            <Button size="lg" className="w-full" disabled={!available || running || !chosen.length} onClick={go}>
              <SwapIcon on={running} active={<Loader2 className="motion-safe:animate-spin" />} inactive={<Play className="translate-x-px" />} />
              {running ? "Running…" : `Run ${chosen.join(" + ") || "attack"}`}
            </Button>
          </SheetFooter>
        </SheetContent>
      </Sheet>
    </Ctx.Provider>
  );
}
