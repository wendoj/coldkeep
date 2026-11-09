# Governor safety rules (G1–G6)

Illustrative food-safety envelope enforced by `governord`. Code: [`core/governor.c`](../core/governor.c) (G1–G5), [`core/control.c`](../core/control.c) (G6). Tests: [`core/tests/test_governor.c`](../core/tests/test_governor.c), [`core/tests/test_control.c`](../core/tests/test_control.c), [`core/tests/test_property.c`](../core/tests/test_property.c).

The governor never trusts the `user` field from `mgmtd`. The envelope is compiled in (`ck_envelope_default`) and only `ck_gov_apply_approved()` changes it, which requires a keyconsole approval id.

## Envelope (defaults)

| Case type | Setpoint band | Default | G4 warning (product) | Food-safe limit (product) | Max defrost | Defrost termination (coil) |
|---|---|---|---|---|---|---|
| Chilled multideck | 0 to 5 °C | 3 °C | 6 °C | 8 °C | 45 min | 10 °C |
| Frozen well | −25 to −20 °C | −22 °C | −19 °C | −18 °C | 60 min | 10 °C |
| Walk-in cold room | 0 to 3 °C | 2 °C | 4 °C | 5 °C | 45 min | 8 °C |

Store-wide: deny margin 10 °C (G1), max step ±2 °C and 2 changes per case per sim-hour (G2), 4 h between manual defrosts and at most 25 % of cases defrosting at once (G3), G4 clears 1 °C below the warning.

## Rules

| ID | Shown in the UI as | Behaviour |
|---|---|---|
| **G1** | *Setpoint band.* The setpoint must stay within the band for the case type. | Outside the band → `CLAMP` to the nearest bound. More than 10 °C outside → `DENY`. |
| **G2** | *Rate limit.* At most 2 setpoint changes per case per sim-hour, at most ±2 °C each. | Bigger step or too many changes → `DENY`. A request equal to the current setpoint is not a change. The step is measured after G1 clamping. |
| **G3** | *Defrost envelope.* Max duration per type, 4 h between manual defrosts, ≤ 25 % of cases at once. | Too long → `CLAMP` to the maximum. Too soon, already defrosting, or too many at once → `DENY`. Every defrost the governor sends carries a coil-temperature termination. Scheduled defrosts skip the 4 h manual gap but not the concurrency limit. |
| **G4** | *Product-temperature override.* If product passes the warning threshold, the governor ends any defrost, forces cooling and restores the last-known-good setpoint. | Automatic, logged with origin `governor`. While active: setpoint raises and defrost starts are `DENY` (G4). Last-known-good is a setpoint that has held for 1 h with product at least 2 °C under the warning (initially the default). |
| **G5** | *Dangerous changes need the store key.* | Envelope change, alarm disable, defrost-schedule change, raw write and **fans off** → `NEEDS_KEY`. The broker queues them for the local keyconsole; they are never applied from the network. The governor refuses them too (defence in depth), whatever `origin` claims. |
| **G6** | *Fail-safe hold.* If the governor is unavailable, the case controller holds its last-known-good configuration and never starts a defrost on its own. | In `casectl`: heartbeat missing > 2 s → hold. Ends any defrost in progress, keeps cooling at the last governor-approved setpoint, and refuses `START_DEFROST` (`EPERM`) until heartbeats return. |

`STOP_DEFROST` and fans **on** are always allowed (safe direction).

## Invariants (property-tested)

Under any random command stream from an authenticated `mgmtd` session, in protected mode, including one governor crash per run (see [`test_property.c`](../core/tests/test_property.c)):

1. Product never exceeds its food-safe limit (normal ambient, seeded shopper load).
2. No defrost cycle runs longer than the G3 maximum.
3. The envelope never changes without a keyconsole approval.

`make proptest-full` runs 10,000 runs × 1,000 commands. Half the runs use a greedy attacker: it always asks for +2 °C and maximum defrosts on random cases, and turns fans off.

**The tests can fail.** We ran them against weakened governors (host only, not committed). Each weakening below is caught by invariant 1:
- the governor sends defrosts without termination;
- fans-off no longer needs the key;
- the setpoint band is 6 °C too wide **and** G4 is disabled.

A too-wide band with G4 still active is **not** caught. That is expected: G4 restores a safe setpoint first.

## Design notes

- **Frozen band −25 to −20 °C**, not −25 to −18 °C. With the setpoint at −18 °C, product sits at about −18 °C ± noise, so invariant 1 (never above −18 °C) could not hold. The bands keep a 2–3 °C margin under each limit.
- **G4 warning thresholds are below the limits** (frozen −19 °C, cold room 4 °C). A warning above the legal limit could never protect the invariant.
- **Defrost termination by coil temperature** is how real controllers end normal defrosts. The governor always attaches it. The legacy flat API's "manual defrost" is a *forced* defrost with no termination, which is how a heater can run until the case reaches tens of °C. As a result, the G3 maximum duration is a backstop: in protected runs, termination normally ends a defrost after 3–12 sim-minutes.
- **Fans off is classed as dangerous** (G5). It cuts cooling as surely as a high setpoint.
