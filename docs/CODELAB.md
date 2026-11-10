# Codelab — build and verify COLDKEEP's QNX boundary

**What you will do:** build COLDKEEP, run the legacy and protected stores on QNX 8.0 in QEMU, and verify from the target itself that a compromised management process cannot reach the plant. Then send the same commands from the dashboard and watch the governor decide.

**Who it is for:** developers who know Linux and C, and want a concrete example of QNX abilities, resource managers and message passing used as a security boundary.

**Time:** 15 minutes for the host-only path; 60–90 minutes for the full QNX path (mostly image build and boot).

**You need for the full path:** QNX SDP 8.0 at `~/qnx800` with the Linux host tools (free non-commercial licence), Docker Desktop, Homebrew `qemu`, Node 20+, and a C11 compiler.

> **Safety.** COLDKEEP is a defensive prototype with a simulated plant. It is not a real refrigeration controller and is not certified for food-safety or industrial use. Run it only in a VM or on a spare machine.

---

## Step 0 — Get the code

```sh
git clone https://github.com/wendoj/coldkeep.git
cd coldkeep
```

Checkpoint: `ls` shows `core/`, `target/`, `host/`, `scripts/` and `docs/`.

## Step 1 — Prove the safety logic on the host (no QNX)

```sh
make test
make demo
```

`make test` runs the portable core's unit and property tests (203,704 unit checks, plus 10,000 random command streams of 1,000 commands). `make demo` runs the host simulator: a legacy store and a protected store receive the same A1–A4 attacks at the same sim times.

Checkpoint: the transcript contains lines like

```text
[A1] t=601 start_defrost case=1 seconds=86400 -> legacy: APPLIED | coldkeep: CLAMP (G3 max defrost 45 min)
```

and the summary at the end shows the legacy store above the limit and the protected store at zero case-minutes above the limit.

**Why this matters:** the governor is ordinary C11 with no QNX headers. You can test the safety rules before touching the target.

## Step 2 — Build the QNX image and boot two stores

```sh
bash docker/setup.sh
scripts/qemu-image.sh ~/coldkeep-qemu/store-a store-a
scripts/qemu-image.sh ~/coldkeep-qemu/store-b store-b
scripts/qemu-run.sh ~/coldkeep-qemu/store-a --slot 0 --headless   # protected
scripts/qemu-run.sh ~/coldkeep-qemu/store-b --slot 1 --headless   # legacy
```

The two VMs get separate forwarded ports:

| Slot | Store | SSH | mgmtd (8080) | bridge (7070) |
|---|---|---|---|---|
| 0 | protected | 2222 | 18080 | 17070 |
| 1 | legacy | 2223 | 18081 | 17071 |

Checkpoint: `ssh -p 2222 root@127.0.0.1 'uname -a'` prints a QNX 8.0 kernel line, and `scripts/qemu-run.sh ... --stop` shuts a VM down.

## Step 3 — Cross-compile and deploy

```sh
scripts/qnx-build.sh target
scripts/deploy.sh 0 protected
scripts/deploy.sh 1 legacy
```

`deploy.sh` copies every process and `target/scripts/coldkeep-start.sh` to `/data/coldkeep/bin`, then starts the store.

Checkpoint: on slot 0,

```sh
ssh -p 2222 root@127.0.0.1 'pidin -f anU | grep -E "mgmtd|governord|casectl|keyconsole"'
```

shows `mgmtd` as uid 1105, `governord` as uid 1103, `casectl` as uid 1102 and `keyconsole` as uid 1108. The priorities in `pidin` should match [ARCHITECTURE.md](ARCHITECTURE.md): `casectl` 40, `governord` 35, `brokerd` 25, `mgmtd` 10.

## Step 4 — Look at the boundary

On the protected store:

```sh
ssh -p 2222 root@127.0.0.1
ls -l /dev/coldkeep
pidin -f anU | grep -E 'mgmtd|governord'
```

Checkpoint: `/dev/coldkeep/gov` is owned by `brokerd`'s uid with mode `0600`, `/dev/coldkeep/broker` by `mgmtd`'s uid, and `keyconsole`'s `/dev/coldkeep/key` is root-only. The management process owns only the broker channel; it does not own the governor or the controllers.

**Why this matters:** on QNX, the file mode and owner decide who can connect at all. The ability check is a second gate on top of that.

## Step 5 — Compromise the web process (A3)

Still on the protected store, run these as `mgmtd`'s uid 1105:

```sh
on -u 1105 sh -c 'echo setpoint 30 | dd of=/dev/coldkeep/case/1 2>&1'
on -u 1105 sh -c 'cat /dev/coldkeep/gov 2>&1'
on -u 1105 sh -c ': >> /data/coldkeep/audit.bin 2>&1'
on -u 1105 sh -c 'cat /data/coldkeep/gov/state > /dev/null 2>&1'
```

Checkpoint: expect these QNX errors:

```text
dd: /dev/coldkeep/case/1: write error: Operation not permitted
cat: /dev/coldkeep/gov: Permission denied
sh: cannot create /data/coldkeep/audit.bin: Permission denied
cat: /data/coldkeep/gov/state: Permission denied
```

Then run the first two on the legacy store (slot 1), where `mgmtd` is root:

```sh
ssh -p 2223 root@127.0.0.1
echo setpoint 30 | dd of=/dev/coldkeep/case/1
: >> /data/coldkeep/audit.bin
```

Checkpoint: both succeed. The governor paths do not exist on the legacy store.

**Why this matters:** the same code, the same password, the same shell — the only difference is what QNX lets the process open.

## Step 6 — Send the same command from the dashboard

Start the dashboard and a live relay with management endpoints:

```sh
cd host
npm ci
npm run build
npm run start -w @coldkeep/dashboard
```

In a second terminal:

```sh
cd host
npm run relay -- --store legacy=127.0.0.1:17071 --store protected=127.0.0.1:17070 \
  --mgmt legacy=127.0.0.1:18081 --mgmt protected=127.0.0.1:18080
```

Open <http://127.0.0.1:3000/?relay=ws://127.0.0.1:7071> and select **Control**.

1. Click **Setpoint +25 °C**. Checkpoint: the legacy store applies it; the Coldkeep store returns `DENY`, rule `G1`, "far outside band".
2. Click **24 h defrost**. Checkpoint: the legacy store applies it; Coldkeep returns `CLAMP`, rule `G3`, "max defrost 45 min" — unless the case is inside the 4-hour manual-defrost gap, in which case it returns `DENY G3` with that reason. Both are correct.
3. Click **Alarms off**. Checkpoint: Coldkeep returns `NEEDS_KEY G5`. Open **Key requests**: the request is listed, and there is no approve button.

**Why this matters:** the dashboard is using the same authenticated `mgmtd` API as the attack runner. It gets no privileged path. The verdicts are the target's, not the UI's.

## Step 7 — Approve only at the store

On the protected store, read the queue and approve the request by id:

```sh
cat /dev/coldkeep/key
echo "approve 1" > /dev/coldkeep/key      # use the id printed above
cat /dev/coldkeep/key
```

Checkpoint: the request moves from `pending` to `approved`, and the audit ring records the approval with origin `keyconsole`.

**Why this matters:** `keyconsole` is the only process holding `coldkeep.envelope.write`. The network process cannot obtain or forge that ability, so it cannot approve its own request even if it finds a path to the governor.

## Step 8 — Crash and freeze the governor (A4)

```sh
slay -s KILL governord; sleep 1
grep 'governord restarted' /data/coldkeep/log/sup.log | tail -1
```

Checkpoint: the supervisor log reports a restart ready in about 30 ms (29.6 ms mean over the recorded runs). Send a renewed 24-hour defrost from the dashboard; it is still `DENY G3`, because the governor restored its state from disk.

Then freeze it:

```sh
slay -s STOP governord; sleep 5; slay -s CONT governord
grep -e 'G6 hold' -e 'G6 released' /dev/coldkeep/audit
```

Checkpoint: all 8 cases log a G6 hold, then a release. In the recorded runs the first hold appeared about 2.18 s after the freeze (heartbeat timeout 2 s).

**Why this matters:** a governor that is only correct while running is not a safety control. Persisted state and controller-side G6 close the "just crash it" and "just freeze it" attacks.

## Step 9 — Record your own comparison

```sh
scripts/run-compare.py --label mylab
scripts/summarize-runs.py recordings/mylab
```

The runner restarts both stores with the same seed and speed and runs A1–A4 at scripted sim times. Read the summary, not the console: A2 jitter and A4 timings are measured from the target's own counters and audit ring.

Checkpoint: the summary has `minutes_above_total`, `a1_applied`, `a1_blocked`, `a3_succeeded`, `restart_ms`, `hold_cases` and `released_cases` per store. Compare with [RESULTS.md](RESULTS.md); do not expect byte-identical numbers, because QEMU timing follows the wall clock.

## Exercises

1. **Add a rule.** Add a G7 that denies a setpoint change when the door has been open for more than 60 s. Put it in `core/governor.c`, add a unit test in `core/test_governor.c`, and add the rule to `docs/RULES.md`.
2. **Remove the local approval.** Change the dashboard to approve key requests and watch the governor refuse: it checks the sender uid and `coldkeep.envelope.write` on every approval, so the request stays pending.
3. **Break the ability drop.** Remove the `LOCK` from `ck_drop_to` in `target/common/ck_target.c`, rebuild and repeat Step 5. Explain what changed and why the lock matters.
4. **Measure on hardware.** Run the same A2 and A4 scenarios on a physical QNX target and compare jitter with the emulator numbers.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `deploy.sh: missing target/build/x86_64/...` | Target not cross-compiled | `scripts/qnx-build.sh target` |
| `cannot reach mgmtd` in `run-compare.py` | Store not started, or wrong slot/port | `scripts/deploy.sh 0 protected`, check `pidin` and `lsof -i :18080` |
| Dashboard says **Read-only** on Control | Relay started without `--mgmt`, or a replay/simulator source | Restart the relay with both `--mgmt` arguments |
| A3 commands succeed on the protected store | Store booted in legacy mode, or abilities not dropped | Re-run `scripts/deploy.sh 0 protected`; check `mgmtd`'s uid in `pidin` |
| Key request never appears | Alarms were already disabled, or the request was rejected | Reboot the store, then repeat Step 6 |
| G3 returns `DENY` instead of `CLAMP` | A manual defrost ran within the last 4 hours | Wait, or use another case; the reason line says so |

## What you have verified

- The safety governor decides every command; the management API cannot bypass it.
- QNX abilities, uids and file ownership stop a compromised management process from reaching the plant.
- Key requests are approved only at the local console, and the governor re-checks the approval sender.
- The governor survives a crash with its state, and controllers enter a fail-safe hold when it disappears.
- The recorded comparison shows the difference: 75.6 vs 0.0 case-minutes above the limit, 14.0 °C vs 3.2 °C peak product, 17/0 vs 3/14 commands applied/blocked, and 2/2 vs 0/4 privileged actions.

Full methodology and raw files: [RESULTS.md](RESULTS.md). Architecture and process table: [ARCHITECTURE.md](ARCHITECTURE.md). Rules: [RULES.md](RULES.md).
