# Dashboard

The dashboard is a Next.js app in `host/dashboard/`. It shows the same attack on a legacy store and a protected store, and it never invents a number: every figure is derived from relay events or from the run's own `summary` line.

## Data path

```
QNX bridge :7070 ─┐
recordings/*.jsonl ├─► relay :7071 ── WebSocket ──► browser
sim-host/ck-sim ──┘        │
                           ├─ HTTP hello ──► /api/relay ──► System view
                           └─ operator command ──► mgmtd :8080 ──► brokerd ──► governord
```

The browser keeps a direct WebSocket to the relay for events. A Next.js route handler cannot complete an HTTP Upgrade, so proxying the socket through `/api` would add a component that cannot do the one thing the dashboard needs. The route handler is used for the other half: a server-side health check of the relay's HTTP hello.

The lower branch exists only when the relay is started with `--mgmt`. Live operator commands travel from the browser to the relay, then over mgmtd's documented JSON-lines API on the target. They do not take a shortcut into the plant: the request still crosses `brokerd` and is decided by `governord`. The reply is matched to the request id and shown next to the command that caused it.

## Frontend and backend

| Layer | File | Responsibility |
|---|---|---|
| Relay | `host/relay/` | Owns the source (live, replay, simulator), broadcasts typed events, accepts a small set of `ClientMessage` requests |
| Operator client | `host/relay/src/operator.ts` | Validates and serialises dashboard commands per store, logs in to mgmtd, reconnects once, returns mgmtd's reply |
| Shared model | `host/shared/` | Zod schemas for every event, `StoreModel`, `computeReadouts()`, `ClientMessage` / `RelayMessage` |
| WebSocket client | `dashboard/lib/relay.tsx` | Reconnects, applies events to one `StoreModel` per store, re-renders at most once per frame |
| Command hook | `dashboard/lib/commands.ts` | Tracks in-flight commands, matches `command_result` by id, times out after 15 s |
| Server route | `dashboard/app/api/relay/route.ts` | Fetches the relay's HTTP hello and returns source, stores, files and capabilities |
| Views | `dashboard/app/*/page.tsx` | Overview, Cases, Commands, Control, Audit, Key requests, System |

The browser can only send the four requests in `ClientMessage`: run a campaign, pause/resume/restart playback, change the playback rate, and send one operator command. Anything else is ignored by `Hub`. The dashboard has no approve control for key requests; approval happens at the store's local console.

### Operator control (live only)

Start the relay with one `--mgmt` endpoint per store:

```sh
cd host
npm run relay -- --store legacy=127.0.0.1:17071 --store protected=127.0.0.1:17070 \
  --mgmt legacy=127.0.0.1:18081 --mgmt protected=127.0.0.1:18080
```

The relay advertises `can.control: ["legacy", "protected"]` in its hello. Without `--mgmt`, or when the source is a recording or the host simulator, `can.control` is absent and the Control view is read-only: the form, presets and Send button are disabled. This is deliberate, so a replay can never look like a live command.

The view exposes only operator-facing commands: `set_setpoint`, `start_defrost`, `stop_defrost`, `set_fans` and `disable_alarms`. Login, `status`, `debug_exec`, envelope changes, defrost-schedule changes and raw writes stay out of the UI. Commands are sent to both stores for the A/B demo; presets compute a per-store setpoint so "+25 °C" means 25 degrees above that store's own value.

Replies are mgmtd's decision (`ALLOW`, `CLAMP`, `DENY`, `NEEDS_KEY`), mapped onto the same badge the Commands view uses. `NEEDS_KEY` links to Key requests and says "queued at the store console". There is no approve path from this view, by design.

### Environment

| Variable | Used by | Default | Meaning |
|---|---|---|---|
| `NEXT_PUBLIC_RELAY_URL` | browser | `ws://127.0.0.1:7071` | WebSocket endpoint; `?relay=ws://…` overrides it for a tab |
| `RELAY_HTTP_URL` | `/api/relay` | `NEXT_PUBLIC_RELAY_URL` with `ws` changed to `http` | Relay HTTP hello used for the System view |

For a deployed dashboard, terminate both the WebSocket and the HTTP route at the same reverse proxy (Caddy or nginx) so the browser and the server route reach the same relay. The relay itself still binds to localhost.

## Visual system

The layout follows the free BoardUI dashboard language: a floating sidebar, a quiet page background, white cards with hairline borders, compact stat cards, and data-dense tables. It is an adaptation, not a dependency:

- no BoardUI package, runtime, Pro components or templates are installed;
- the app keeps its existing shadcn/Radix primitives and Tailwind v4 tokens;
- only free-tier patterns are used (app shell, sidebar, stat cards, table and chip patterns);
- labels use sentence case, not all-caps eyebrows or wide tracking;
- status is always icon + label + number, never colour alone;
- the source of the data (host simulator, QEMU recording, live targets) stays visible in the header.

Tokens live in `app/globals.css`. `surface` is the one card treatment; `Card` and `PageHeader` in `components/kit.tsx` are the shared building blocks.

## Views

| View | What it answers |
|---|---|
| Overview | Did product stay inside the limit, what did the attacker try, and which rules stopped it |
| Cases | Product temperature, status and active plant states for every case in both stores |
| Commands | Every command paired with both verdicts, plus privileged attempts from inside the web process |
| Control | Send individual operator commands to live targets and see each store's verdict; read-only in replay/simulator mode |
| Audit | Searchable decisions, 15-minute compliance records, events, CSV and incident report |
| Key requests | Queued G5 changes and their state at the local console; no approve control |
| System | Relay health, store feeds, process events, governor status and the data path |

## Tests

```sh
cd host
npm run build -w @coldkeep/dashboard
npm run e2e -w @coldkeep/dashboard
```

The Playwright suite runs against a recorded QEMU run and the host simulator. It checks the readouts against the recording's `summary` line, the paired command table, the case grid, key requests, running a new attack from the dashboard, and that Control is read-only against a recording (form and presets disabled, no command table). The relay suite includes operator-command tests against a mock mgmtd: a decision is echoed with the request id, `NEEDS_KEY` is never presented as an approval, an unreachable target becomes an error reply, and a store outside `can.control` never reaches mgmtd.
