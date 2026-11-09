# Threat model

> COLDKEEP is a defensive research prototype built for a hackathon. It uses a simulated refrigeration plant, is not certified for food-safety or industrial use, and does not describe or reproduce any real-world exploit.

## Assets
- **Food safety:** product temperatures stay under their limits.
- **Plant integrity:** only the governor commands the case controllers.
- **Audit integrity:** every decision is recorded, and the trusted zone controls the journal.

## Trust boundary
`mgmtd` (the network-facing API) is **assumed compromised**. Everything it sends is untrusted, including the claimed `user`. The trusted zone is `brokerd`, `governord`, `casectl`, `plantsim`, `auditd` and `keyconsole`. On QNX this boundary is enforced by abilities, uid and (if available) security policy.

## Attacker capabilities (A1–A4)
| ID | Capability | Defence |
|---|---|---|
| A1 | Valid admin credentials (the default `admin/admin`, deliberately) and the full documented API | Broker classification + governor G1–G5 |
| A2 | Unlimited request rate | Per-session rate limit; real-time priorities on QNX (to be measured) |
| A3 | Code execution inside `mgmtd` (simulated switch, no real exploit) | Kernel-enforced: abilities dropped and locked, custom ability `coldkeep.plant.write`, security policy. *(In sim-host this is a model; on the target it is QNX.)* |
| A4 | Crash of `governord` | Supervisor restart; G6 hold in `casectl` |

## Not defended (by design)
- Physical access to the store or the QNX node.
- Compromise of a trusted process (`governord`, `casectl`, …).
- Kernel or hypervisor bugs.
- A malicious operator at the local keyconsole.

## Legacy-mode honesty note
Legacy mode is a **deliberately simplified model of a flat architecture**: the management interface writes the controllers directly. It models the reported root cause (whoever controls the management interface controls the cold). It is not a copy of any vendor's product.

