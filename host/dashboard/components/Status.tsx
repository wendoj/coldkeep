// SPDX-License-Identifier: Apache-2.0
import type { CaseStatus } from "@/lib/derive";

const LABEL: Record<CaseStatus, string> = { ok: "In range", warn: "Warning", over: "Over limit" };

/** Status is never colour alone: icon + label + colour. */
export function StatusBadge({ status }: { status: CaseStatus }) {
  return (
    <span className="status" data-status={status}>
      <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">
        {status === "ok" && (
          <>
            <circle cx="7" cy="7" r="6.5" fill="var(--good)" />
            <path d="M4 7.2l2 2 4-4.4" stroke="#fff" strokeWidth="1.8" fill="none" strokeLinecap="round" strokeLinejoin="round" />
          </>
        )}
        {status === "warn" && (
          <>
            <path d="M7 1l6.3 11.5H.7z" fill="var(--warning)" />
            <path d="M7 5v3.6" stroke="#0b0b0b" strokeWidth="1.6" strokeLinecap="round" />
            <circle cx="7" cy="10.6" r="0.9" fill="#0b0b0b" />
          </>
        )}
        {status === "over" && (
          <>
            <rect x="0.5" y="0.5" width="13" height="13" rx="3" fill="var(--critical)" />
            <path d="M4.3 4.3l5.4 5.4M9.7 4.3l-5.4 5.4" stroke="#fff" strokeWidth="1.8" strokeLinecap="round" />
          </>
        )}
      </svg>
      {LABEL[status]}
    </span>
  );
}
