// SPDX-License-Identifier: Apache-2.0
//
// The dashboard talks to the relay over WebSocket for live events. This small
// server-side companion endpoint exposes the relay's HTTP hello (source,
// stores, capabilities) so the UI can show backend health without opening a
// second socket. See docs/DASHBOARD.md.
import type { RelayMessage } from "@coldkeep/shared";

export type RelayHello = Extract<RelayMessage, { kind: "hello" }>;

export interface RelayHealth {
  ok: boolean;
  url: string;
  hello?: RelayHello;
  error?: string;
}

export async function fetchRelayHealth(): Promise<RelayHealth> {
  try {
    const res = await fetch("/api/relay", { cache: "no-store" });
    const body = (await res.json()) as RelayHealth;
    return res.ok ? body : { ok: false, url: body.url ?? "", error: body.error ?? `HTTP ${res.status}` };
  } catch (e) {
    return { ok: false, url: "", error: (e as Error).message };
  }
}
