"use client";
// SPDX-License-Identifier: Apache-2.0
import Link from "next/link";
import { usePathname } from "next/navigation";
import type { ReactNode } from "react";
import { storeColor, storeLabel, useRelay } from "@/lib/relay";

const NAV = [
  ["/", "Store"],
  ["/compare", "Compare"],
  ["/attacks", "Attack console"],
  ["/audit", "Audit & compliance"],
  ["/keys", "Key requests"],
] as const;

export function Shell({ children }: { children: ReactNode }) {
  const path = usePathname();
  const r = useRelay();
  return (
    <>
      <header className="topbar">
        <div className="brand">
          COLDKEEP<small>refrigeration safety supervisor</small>
        </div>
        <nav className="nav" aria-label="Views">
          {NAV.map(([href, label]) => (
            <Link key={href} href={href} aria-current={path === href ? "page" : undefined}>
              {label}
            </Link>
          ))}
        </nav>
        <div className="conn" data-testid="connection">
          <span>
            <span className="dot" style={{ background: r.connected ? "var(--good)" : "var(--critical)" }} />
            {r.connected ? `relay ${r.source ?? ""}` : `relay offline (${r.url})`}
          </span>
          {(Object.keys(r.stores) as (keyof typeof r.stores)[]).map((m) => {
            const st = r.storeStatus[m];
            return (
              <span key={m} title={st?.detail}>
                <span className="store-key" style={{ background: storeColor(m) }} />
                {storeLabel(m)}
                {r.ended[m] ? " · ended" : st && !st.connected ? " · disconnected" : ""}
              </span>
            );
          })}
        </div>
      </header>
      <main>{children}</main>
      <footer className="footer">
        COLDKEEP is a defensive research prototype with a simulated refrigeration plant. It is not certified for
        food-safety or industrial use and does not describe or reproduce any real-world exploit. Food quantities are
        illustrative.
      </footer>
    </>
  );
}

export function Waiting({ what = "data" }: { what?: string }) {
  const r = useRelay();
  return (
    <div className="panel empty">
      {r.connected ? `Waiting for ${what} from the relay…` : (
        <>
          No relay at <code>{r.url}</code>. Start one, for example{" "}
          <code>npm run relay -- --replay ../docs/evidence/runs/qemu-run1.legacy.jsonl --replay ../docs/evidence/runs/qemu-run1.protected.jsonl</code>
        </>
      )}
    </div>
  );
}
