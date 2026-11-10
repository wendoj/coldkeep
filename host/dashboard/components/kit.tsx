"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Small building blocks shared by the views.
import type { Decision, Mode } from "@coldkeep/shared";
import type { ReactNode } from "react";
import { Badge } from "@/components/ui/badge";
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import { RULE_NAME, verdictText } from "@/lib/derive";
import { storeLabel } from "@/lib/relay";
import { cn } from "@/lib/utils";

const swapLayer =
  "[grid-area:1/1] transition-[opacity,filter,scale] duration-300 ease-[cubic-bezier(0.2,0,0,1)] motion-reduce:scale-100 motion-reduce:blur-[0px]";
const swapShown = "scale-100 opacity-100 blur-[0px]";
const swapHidden = "scale-[0.25] opacity-0 blur-[4px]";

/** Cross-fades between two icons on an infrequent state change (play/pause, idle/running). */
export function SwapIcon({ on, active, inactive }: { on: boolean; active: ReactNode; inactive: ReactNode }) {
  return (
    <span className="grid place-items-center" data-icon="inline-start">
      <span className={cn(swapLayer, on ? swapShown : swapHidden)} aria-hidden={!on}>{active}</span>
      <span className={cn(swapLayer, on ? swapHidden : swapShown)} aria-hidden={on}>{inactive}</span>
    </span>
  );
}

/** One block of a page: title, optional actions on the right, content. */
export function Section({
  title,
  actions,
  children,
  className,
  ...rest
}: { title?: ReactNode; actions?: ReactNode; children: ReactNode; className?: string } & Omit<React.ComponentProps<"section">, "title">) {
  return (
    <section className={cn("min-w-0 p-5 sm:p-6", className)} {...rest}>
      {(title || actions) && (
        <header className="mb-5 flex min-h-8 flex-wrap items-center gap-3">
          {title && <h2 className="text-base font-semibold tracking-tight">{title}</h2>}
          {actions && <div className="ml-auto flex flex-wrap items-center gap-2">{actions}</div>}
        </header>
      )}
      {children}
    </section>
  );
}

/** A bordered panel: a title row with optional actions, then the body. */
export function Card({
  title,
  actions,
  children,
  className,
  bodyClassName,
  ...rest
}: {
  title?: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
  className?: string;
  bodyClassName?: string;
} & Omit<React.ComponentProps<"section">, "title">) {
  return (
    <section className={cn("min-w-0 rounded-md border bg-card", className)} {...rest}>
      {(title || actions) && (
        <header className="flex min-h-11 flex-wrap items-center gap-3 border-b px-4 py-2">
          {title && <h2 className="text-sm font-medium">{title}</h2>}
          {actions && <div className="ml-auto flex flex-wrap items-center gap-2">{actions}</div>}
        </header>
      )}
      <div className={cn("p-4", bodyClassName)}>{children}</div>
    </section>
  );
}

/** Page title row used at the top of every view. */
export function PageHeader({ title, actions, className }: { title: ReactNode; actions?: ReactNode; className?: string }) {
  return (
    <header className={cn("flex flex-wrap items-center justify-between gap-4", className)}>
      <h1 className="text-xl font-medium tracking-[-0.01em]">{title}</h1>
      {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
    </header>
  );
}

export function Eyebrow({ children, className }: { children: ReactNode; className?: string }) {
  return <div className={cn("mono-label", className)}>{children}</div>;
}

/** A thin filled bar used for rule counts and command outcomes. */
export function Bar({ value, max, className, label }: { value: number; max: number; className?: string; label?: string }) {
  const pct = max > 0 ? Math.max(0, Math.min(100, (value / max) * 100)) : 0;
  return (
    <span className={cn("block h-1 overflow-hidden bg-muted", className)} aria-label={label}>
      <span className={cn("block h-full bg-coldkeep transition-[width] duration-500 ease-out")} style={{ width: `${pct}%` }} />
    </span>
  );
}

export function StoreDot({ mode, className }: { mode: Mode; className?: string }) {
  return (
    <span
      aria-hidden
      className={cn("inline-block size-2 shrink-0 rounded-[1px]", mode === "legacy" ? "bg-legacy" : "bg-coldkeep", className)}
    />
  );
}

export function StoreName({ mode }: { mode: Mode }) {
  return (
    <span className="inline-flex items-center gap-2">
      <StoreDot mode={mode} />
      {storeLabel(mode)}
    </span>
  );
}

const TONE = {
  bad: "border-foreground/25 bg-foreground/[0.04] text-foreground",
  good: "border-good/30 bg-primary/60 text-good-ink",
  key: "border-input bg-muted text-warn-ink",
  neutral: "border-border bg-muted text-muted-foreground",
} as const;

export function Pill({ tone, className, ...rest }: { tone: keyof typeof TONE } & React.ComponentProps<typeof Badge>) {
  return <Badge className={cn("rounded-[2px] border font-mono text-[10.5px] font-normal tracking-[0.04rem] uppercase", TONE[tone], className)} {...rest} />;
}

const VERDICT_TONE: Record<string, keyof typeof TONE> = {
  APPLIED: "bad",
  ALLOW: "neutral",
  CLAMP: "good",
  DENY: "good",
  NEEDS_KEY: "key",
  FAILED: "neutral",
};
const VERDICT_WORD: Record<string, string> = {
  APPLIED: "Applied",
  ALLOW: "Allowed",
  CLAMP: "Clamped",
  DENY: "Denied",
  NEEDS_KEY: "Needs key",
  FAILED: "Failed",
};

/** A store's verdict on one command; the rule and its reason on hover. */
export function VerdictBadge({ d }: { d: Decision | undefined }) {
  if (!d) return <span className="text-muted-foreground">—</span>;
  const v = verdictText(d);
  const badge = (
    <span className="inline-flex items-center gap-1.5">
      <Pill tone={VERDICT_TONE[v] ?? "neutral"} className="verdict" data-v={v}>
        {VERDICT_WORD[v] ?? v}
      </Pill>
      {d.rule !== "LEGACY" && /^G\d$/.test(d.rule) && <span className="font-mono text-xs text-muted-foreground">{d.rule}</span>}
    </span>
  );
  if (d.rule === "LEGACY" || !d.reason) return badge;
  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <span tabIndex={0} className="cursor-help rounded-md">{badge}</span>
      </TooltipTrigger>
      <TooltipContent className="max-w-72">
        <div className="font-medium">{RULE_NAME[d.rule] ?? d.rule}</div>
        <div className="opacity-80">{d.reason}</div>
      </TooltipContent>
    </Tooltip>
  );
}

/**
 * Result of a privileged attempt: SUCCEEDED is bad; anything else is the refusal
 * QNX gave (shown short, e.g. "Operation not permitted", with the full text on hover).
 */
export function ResultBadge({ result }: { result: string | undefined }) {
  if (!result) return <span className="text-muted-foreground">—</span>;
  if (result.startsWith("n/a")) return <span className="text-xs text-muted-foreground">Not applicable</span>;
  if (result === "SUCCEEDED") return <Pill tone="bad">Succeeded</Pill>;
  const short = result.split(": ").at(-1) ?? result;
  return (
    <Pill tone="good" title={result} className="max-w-full justify-start">
      <span className="truncate">{short}</span>
    </Pill>
  );
}

export function StoreToggle({ stores, value, onChange }: { stores: Mode[]; value: Mode; onChange: (m: Mode) => void }) {
  return (
    <ToggleGroup
      type="single"
      variant="outline"
      size="sm"
      value={value}
      onValueChange={(v) => v && onChange(v as Mode)}
      aria-label="Store"
    >
      {stores.map((m) => (
        <ToggleGroupItem key={m} value={m} className="gap-2 px-3 data-[state=on]:bg-muted">
          <StoreDot mode={m} />
          {storeLabel(m)}
        </ToggleGroupItem>
      ))}
    </ToggleGroup>
  );
}
