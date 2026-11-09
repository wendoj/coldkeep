# Threat model

> COLDKEEP is a defensive research prototype built for a hackathon. It uses a simulated refrigeration plant, is not certified for food-safety or industrial use, and does not describe or reproduce any real-world exploit.

## Assets
- **Food safety:** product temperatures stay under their limits.
- **Plant integrity:** only the governor commands the case controllers.
- **Audit integrity:** every decision is recorded, and the trusted zone controls the journal.

## Trust boundary
`mgmtd` (the network-facing API) is **assumed compromised**. Everything it sends is untrusted, including the claimed `user`. The trusted zone is `brokerd`, `governord`, `casectl`, `plantsim`, `auditd` and `keyconsole`. On QNX this boundary is enforced by abilities, uid and file ownership. A QNX security policy is not used yet, because it needs a policy file ([QNX_FEATURES.md](QNX_FEATURES.md), question 3).

## Attacker capabilities (A1–A4)
| ID | Capability | Defence |
|---|---|---|
| A1 | Valid admin credentials (the default `admin/admin`, deliberately) and the full documented API | Broker classification + governor G1–G5 |
| A2 | Unlimited request rate | Per-session rate limit. `mgmtd` runs at priority 10, below `casectl` (40). On the target, a flood that saturated `mgmtd` did not increase loop jitter ([RESULTS.md](RESULTS.md)). `mgmtd` ignores SIGPIPE, so a client that hangs up cannot kill it, and `sup` restarts it if it dies. |
| A3 | Code execution inside `mgmtd` (simulated switch, no real exploit) | Kernel-enforced: abilities dropped and locked, custom ability `coldkeep.plant.write`, uid and file ownership on every channel. On the target, 0 of the 4 attempts succeeded in all 5 recorded runs ([RESULTS.md](RESULTS.md)). *(In sim-host this is a model.)* |
| A4 | Crash or hang of `governord` | `sup` restarts it (about 30 ms on the target), and it restores its state from disk, so G2/G3 history survives. If the governor hangs, `casectl` holds its last-known-good configuration after 2 s without heartbeats (G6). All recorded on the target ([RESULTS.md](RESULTS.md)). |

## Store key (G5)
Dangerous changes are queued in `keyconsole`, never applied from the network. This covers alarms, the envelope and the defrost schedule.

- Only root at the store can read or write `/dev/coldkeep/key` (mode 0600).
- `governord` accepts an approval only on `/dev/coldkeep/gov-key`, whose owner is `keyconsole`'s uid (mode 0600).
- On every approval message, `governord` checks that the sender is `keyconsole`'s uid **and** holds the custom ability `coldkeep.envelope.write`. Only `keyconsole` keeps that ability.
- On the target, `mgmtd`'s uid gets "Permission denied" on both paths.
- Pending requests live in `keyconsole`'s memory. If it restarts they are lost, which fails safe: nothing is applied.

## Not defended (by design)
- Physical access to the store or the QNX node.
- Compromise of a trusted process (`governord`, `casectl`, …).
- Kernel or hypervisor bugs.
- A malicious operator at the local keyconsole.

## Legacy-mode honesty note
Legacy mode is a **deliberately simplified model of a flat architecture**: the management interface writes the controllers directly. It models the reported root cause (whoever controls the management interface controls the cold). It is not a copy of any vendor's product.

