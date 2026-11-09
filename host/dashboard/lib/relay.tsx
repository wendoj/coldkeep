"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Connects to the host relay and keeps one StoreModel per store. Every number on
// screen is derived from these models, which are built only from relay events.
import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { RelayMessage, StoreModel, type Mode } from "@coldkeep/shared";

const DEFAULT_RELAY = process.env.NEXT_PUBLIC_RELAY_URL ?? "ws://127.0.0.1:7071";

/** Relay URL: ?relay=ws://host:port (remembered for this tab), else the build-time default. */
function relayUrl(): string {
  if (typeof window === "undefined") return DEFAULT_RELAY;
  const q = new URLSearchParams(window.location.search).get("relay");
  try {
    if (q) sessionStorage.setItem("coldkeep.relay", q);
    return q ?? sessionStorage.getItem("coldkeep.relay") ?? DEFAULT_RELAY;
  } catch {
    return q ?? DEFAULT_RELAY;
  }
}

export interface RelayState {
  url: string;
  connected: boolean;
  source: "live" | "replay" | null;
  files: string[];
  stores: Partial<Record<Mode, StoreModel>>;
  storeStatus: Partial<Record<Mode, { connected: boolean; detail?: string }>>;
  ended: Partial<Record<Mode, boolean>>;
  rev: number;
}

const initial = (url = DEFAULT_RELAY): RelayState => ({
  url, connected: false, source: null, files: [], stores: {}, storeStatus: {}, ended: {}, rev: 0,
});

const Ctx = createContext<RelayState>(initial());

export function RelayProvider({ children }: { children: ReactNode }) {
  const [state, setState] = useState<RelayState>(initial);
  const ref = useRef<RelayState>(state);

  useEffect(() => {
    const url = relayUrl();
    let ws: WebSocket | null = null;
    let retry: ReturnType<typeof setTimeout> | null = null;
    let frame: number | null = null;
    let closed = false;
    // events arrive in bursts (backlog, fast replay): re-render at most once per frame
    const publish = () => {
      if (frame !== null) return;
      frame = requestAnimationFrame(() => {
        frame = null;
        ref.current = { ...ref.current, rev: ref.current.rev + 1 };
        setState(ref.current);
      });
    };
    const connect = () => {
      ws = new WebSocket(url);
      ws.onopen = () => {
        ref.current = { ...initial(url), connected: true };
        publish();
      };
      ws.onmessage = (e) => {
        const r = RelayMessage.safeParse(JSON.parse(String(e.data)));
        if (!r.success) return;
        const m = r.data;
        const s = ref.current;
        if (m.kind === "hello") {
          const stores: RelayState["stores"] = {};
          for (const st of m.stores) stores[st] = new StoreModel(st);
          ref.current = { ...s, source: m.source, files: m.files ?? [], stores, ended: {} };
        } else if (m.kind === "event") {
          (s.stores[m.store] ??= new StoreModel(m.store)).apply(m.event);
        } else if (m.kind === "status") {
          s.storeStatus = { ...s.storeStatus, [m.store]: { connected: m.connected, detail: m.detail } };
        } else if (m.kind === "end") {
          s.ended = { ...s.ended, [m.store]: true };
        }
        publish();
      };
      ws.onclose = () => {
        ref.current = { ...ref.current, url, connected: false };
        publish();
        if (!closed) retry = setTimeout(connect, 1500);
      };
    };
    connect();
    return () => {
      closed = true;
      if (retry) clearTimeout(retry);
      if (frame !== null) cancelAnimationFrame(frame);
      ws?.close();
    };
  }, []);

  return <Ctx.Provider value={state}>{children}</Ctx.Provider>;
}

export const useRelay = () => useContext(Ctx);

export const storeLabel = (m: Mode) => (m === "legacy" ? "Legacy (flat)" : "COLDKEEP");
export const storeColor = (m: Mode) => (m === "legacy" ? "var(--series-legacy)" : "var(--series-coldkeep)");

export function fmtSim(t: number): string {
  const s = Math.max(0, Math.round(t));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return `${h}:${String(m).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`;
}

export function download(name: string, text: string, type = "text/plain") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
