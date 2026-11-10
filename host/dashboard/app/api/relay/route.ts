// SPDX-License-Identifier: Apache-2.0
//
// Server-side relay health. The browser keeps its direct WebSocket for events
// (Next route handlers cannot complete an HTTP Upgrade), but this endpoint
// gives the UI one trusted place to ask: which relay am I attached to, what is
// it playing, and what may it do? See docs/DASHBOARD.md.
import { NextResponse } from "next/server";
import { RelayMessage } from "@coldkeep/shared";
import type { RelayHealth } from "@/lib/relay-info";

export const dynamic = "force-dynamic";

const DEFAULT_RELAY = "ws://127.0.0.1:7071";

function httpUrl(raw: string): string {
  return raw.replace(/^ws(s?):\/\//, "http$1://");
}

export async function GET() {
  const url = httpUrl(process.env.RELAY_HTTP_URL ?? process.env.NEXT_PUBLIC_RELAY_URL ?? DEFAULT_RELAY);
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 1500);
  try {
    const res = await fetch(url, { cache: "no-store", signal: controller.signal, headers: { accept: "application/json" } });
    if (!res.ok) {
      return NextResponse.json<RelayHealth>({ ok: false, url, error: `Relay returned HTTP ${res.status}` });
    }
    const parsed = RelayMessage.safeParse(await res.json());
    if (!parsed.success || parsed.data.kind !== "hello") {
      return NextResponse.json<RelayHealth>({ ok: false, url, error: "Relay did not return a Coldkeep hello" });
    }
    return NextResponse.json<RelayHealth>({ ok: true, url, hello: parsed.data });
  } catch (e) {
    const message = (e as Error).name === "AbortError" ? "Relay did not answer within 1.5 s" : (e as Error).message;
    return NextResponse.json<RelayHealth>({ ok: false, url, error: message });
  } finally {
    clearTimeout(timer);
  }
}
