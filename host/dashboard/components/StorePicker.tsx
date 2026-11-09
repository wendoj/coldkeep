"use client";
// SPDX-License-Identifier: Apache-2.0
import type { Mode } from "@coldkeep/shared";
import { storeLabel } from "@/lib/relay";

export function StorePicker({ stores, value, onChange }: { stores: Mode[]; value: Mode; onChange: (m: Mode) => void }) {
  return (
    <div className="seg" role="group" aria-label="Store">
      {stores.map((m) => (
        <button key={m} aria-pressed={m === value} onClick={() => onChange(m)}>
          {storeLabel(m)}
        </button>
      ))}
    </div>
  );
}
