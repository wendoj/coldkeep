// SPDX-License-Identifier: Apache-2.0
import { CircleAlert, CircleCheck, OctagonX } from "lucide-react";
import type { CaseStatus } from "@/lib/derive";
import { cn } from "@/lib/utils";

const LABEL: Record<CaseStatus, string> = { ok: "In range", warn: "Warning", over: "Over limit" };
const ICON = { ok: CircleCheck, warn: CircleAlert, over: OctagonX } as const;
const COLOR: Record<CaseStatus, string> = { ok: "text-good-ink", warn: "text-warn-ink", over: "text-critical-ink" };

/** Status is never colour alone: icon + label + colour. */
export function StatusBadge({ status, compact = false }: { status: CaseStatus; compact?: boolean }) {
  const Icon = ICON[status];
  return (
    <span className={cn("inline-flex items-center gap-1.5 font-mono text-[11px] tracking-[0.04rem] whitespace-nowrap uppercase", COLOR[status])} data-status={status}>
      <Icon className="icon-regular size-3" aria-hidden />
      <span className={cn(compact && "sr-only")}>{LABEL[status]}</span>
    </span>
  );
}

export const STATUS_BAR: Record<CaseStatus, string> = { ok: "bg-good", warn: "bg-warn", over: "bg-critical" };
