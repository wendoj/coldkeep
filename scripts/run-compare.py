#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the same scenario on a legacy and a protected COLDKEEP store on QEMU and
record both.

Both stores are restarted together (same seed, same speed). The runner records
telemetry every 0.25 s wall and runs, at scripted sim times:

  A1  credentialed takeover through the documented API, one request per second
  A2  API flood: casectl loop-jitter histograms are read before a quiet window,
      between it and an equally long flood window, and after the flood. The flood
      comes from ckflood on the target (loopback, mgmtd's uid, default priority),
      because QEMU's user-mode network caps a flood from the host at a few hundred
      requests per second
  A3  the privileged actions a compromised web process would try, run as shell
      commands with mgmtd's uid on the target, so the errors come from QNX itself
  A4  (protected) kill -9 governord: restart time measured by sup; then freeze it
      (SIGSTOP) for 5 s: the controllers must enter G6 hold, then release it

Summaries are computed only from recorded samples.

  scripts/run-compare.py [--label run1] [--seed 7] [--speed 30] [--attack-at 600]
                         [--a2-at 1150] [--a2-seconds 15] [--a3-at 2100] [--a4-at 2700]
                         [--end 3600]   (use a negative time to skip a campaign)
"""
import argparse, datetime, json, os, re, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIMIT = {"chilled": 8.0, "frozen": -18.0, "coldroom": 5.0}
FOOD_KG = {"chilled": 400.0, "frozen": 300.0, "coldroom": 1500.0}

A3_CHECKS = [
    # (name, shell command run as mgmtd's uid)
    ("write case controller", "echo setpoint 30 | dd of=/dev/coldkeep/case/1 2>&1"),
    ("connect governor channel", "cat /dev/coldkeep/gov 2>&1"),
    ("open audit journal for writing", ": >> /data/coldkeep/audit.bin"),
    ("read governor state", "cat /data/coldkeep/gov/state > /dev/null"),
]


JIT_EDGES_US = [100, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000]


def ssh(slot, cmd, timeout=90):
    r = subprocess.run([os.path.join(ROOT, "scripts", "qnx-ssh.sh"), str(slot), cmd],
                       capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).strip()


class Store:
    def __init__(self, mode, slot, args, out_path, meta):
        self.mode, self.slot, self.args = mode, slot, args
        self.label = "legacy" if mode == "legacy" else "coldkeep"
        self.f = open(out_path, "w")
        self.lines = []
        self.transcript = {}
        self.samples = []
        self.decisions = []
        self.a3 = []
        self.a2_result = None
        self.a4_result = None
        self.lock = threading.Lock()
        self.write({"type": "meta", **meta, "mode": mode, "slot": slot})

    def write(self, obj):
        self.f.write(json.dumps(obj) + "\n")
        self.f.flush()

    def connect(self):
        with self.lock:
            self.connect_locked()

    def connect_locked(self):
        for _ in range(60):
            try:
                self.sock = socket.create_connection(("127.0.0.1", 18080 + self.slot), timeout=10)
                self.io = self.sock.makefile("rw")
                self.io.write(json.dumps({"op": "login", "user": "admin", "pass": "admin"}) + "\n")
                self.io.flush()
                if json.loads(self.io.readline() or "{}").get("ok"):
                    return
            except (OSError, ValueError):
                pass
            time.sleep(0.5)
        raise RuntimeError(f"{self.mode}: cannot reach mgmtd")

    def req(self, obj):
        with self.lock:
            for attempt in range(2):
                try:
                    self.io.write(json.dumps(obj) + "\n")
                    self.io.flush()
                    line = self.io.readline()
                    if line:
                        return json.loads(line)
                except OSError:
                    pass
                if attempt == 0:  # mgmtd went away (it is restarted by sup): log in again
                    self.write({"type": "note", "t": self.samples[-1]["t"] if self.samples else 0,
                                "text": "management API connection lost; reconnecting"})
                    self.connect_locked()
            raise RuntimeError(f"{self.mode}: mgmtd not answering")

    def status(self):
        return self.req({"op": "status"})

    def run(self):
        a = self.args
        self.connect()
        last_rec, attacked, a3_done = -1e9, False, False
        side = {"a2": a.a2_at < 0, "a4": a.a4_at < 0}
        workers = []
        while True:
            st = self.status()
            t = st["t"]
            self.samples.append(st)
            if t - last_rec >= 30:
                self.write({"type": "telemetry", "t": t, "cases": st["cases"]})
                last_rec = t
            if not attacked and t >= a.attack_at:
                attacked = True
                self.a1(st)
            if not a3_done and a.a3_at >= 0 and t >= a.a3_at:
                a3_done = True
                self.a3_checks(t)
            for key, at, fn in (("a2", a.a2_at, self.a2), ("a4", a.a4_at, self.a4)):
                if not side[key] and t >= at:
                    side[key] = True
                    th = threading.Thread(target=fn, args=(t,))
                    th.start()
                    workers.append(th)
            if t >= a.end and not any(w.is_alive() for w in workers):
                break
            time.sleep(0.25)
        self.summary()
        self.f.close()

    def record_decision(self, key, r, t):
        if key[0] == "A1":  # the A1 applied/blocked counts cover the 17 A1 commands only
            self.decisions.append(r)
        self.transcript[key] = r
        self.write({"type": "decision", "t": t, **r})

    def a1(self, st):
        for c in st["cases"]:
            r = self.req({"op": "start_defrost", "case": c["id"], "seconds": 86400})
            self.record_decision(("A1", "start_defrost", c["id"]), r, self.status()["t"])
            time.sleep(1)
        for c in st["cases"]:
            v = c["sp"] + 25
            r = self.req({"op": "set_setpoint", "case": c["id"], "value": v})
            self.record_decision(("A1", "set_setpoint", c["id"]), r, self.status()["t"])
            time.sleep(1)
        r = self.req({"op": "disable_alarms"})
        self.record_decision(("A1", "disable_alarms", 0), r, self.status()["t"])

    def mgmt_uid(self):
        rc, out = ssh(self.slot, "pidin -f anU | grep bin/mgmtd")
        return out.split()[-1] if rc == 0 and out else "?"

    def a3_checks(self, t):
        uid = self.mgmt_uid()
        for name, cmd in A3_CHECKS:
            rc, out = ssh(self.slot, f"on -u {uid} sh -c '{cmd}' 2>&1; echo EXIT=$?")
            body, _, code = out.rpartition("EXIT=")
            code = code.strip() or "?"
            lines = body.strip().splitlines()
            err = next((l for l in lines if any(k in l for k in ("denied", "not permitted", "No such"))), "")
            if code == "0":
                result = "SUCCEEDED"
            elif "No such file" in err:
                result = "n/a (does not exist in this architecture)"
            else:
                result = err or f"failed (exit {code})"
            entry = {"type": "attack", "t": t, "campaign": "A3", "action": name, "as_uid": uid,
                     "command": cmd, "exit": code, "result": result, "source": "qnx"}
            self.a3.append(entry)
            self.transcript[("A3", name)] = entry
            self.write(entry)

    # ---- A2: flood vs loop jitter -------------------------------------------------
    def jitter_snapshot(self):
        """Cumulative loop-jitter histogram of every casectl, summed."""
        rc, out = ssh(self.slot, "for f in /dev/coldkeep/case/*; do sed -n 2p $f; done")
        total = [0] * (len(JIT_EDGES_US) + 1)
        for line in out.splitlines():
            m = re.search(r"hist=([\d,]+)", line)
            if m:
                for i, v in enumerate(int(x) for x in m.group(1).split(",")):
                    total[i] += v
        return total

    @staticmethod
    def jitter_stats(before, after):
        h = [b - a for a, b in zip(before, after)]
        n = sum(h)
        if not n:
            return {"ticks": 0}

        def pct(p):  # upper edge of the bucket holding the p-th percentile
            acc = 0
            for i, c in enumerate(h):
                acc += c
                if acc >= p * n:
                    return JIT_EDGES_US[i] if i < len(JIT_EDGES_US) else None
            return None
        over = lambda us: sum(c for i, c in enumerate(h) if i >= JIT_EDGES_US.index(us))
        return {"ticks": n, "hist": h, "p50_le_us": pct(0.5), "p99_le_us": pct(0.99),
                "share_ge_1ms": round(over(1000) / n, 4), "share_ge_5ms": round(over(5000) / n, 4),
                "share_ge_20ms": round(over(20000) / n, 4)}

    def a2(self, t):
        w = self.args.a2_seconds
        s0 = self.jitter_snapshot()
        time.sleep(w)
        s1 = self.jitter_snapshot()
        t_flood = self.status()["t"]
        rc, out = ssh(self.slot, f"on -u {self.mgmt_uid()} /data/coldkeep/bin/ckflood -c 4 -s {w:g}", timeout=w + 60)
        m = re.search(r"(\d+) requests sent, (\d+) replies", out)
        sent, replies = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        s2 = self.jitter_snapshot()
        quiet, flooded = self.jitter_stats(s0, s1), self.jitter_stats(s1, s2)
        self.a2_result = {"window_s": w, "requests": sent, "replies": replies, "quiet": quiet, "flood": flooded}
        entry = {"type": "attack", "t": t_flood, "campaign": "A2", "action": "flood",
                 "result": f"{sent} requests in {w:g} s wall from ckflood on the target, {replies} answered",
                 "source": "qnx", **self.a2_result}
        self.transcript[("A2", "flood")] = entry
        self.write(entry)

    # ---- A4: governor crash and hang (protected only) -------------------------------
    def a4(self, t):
        if self.mode != "protected":
            for action in ("kill governord", "freeze governord 5 s"):
                e = {"type": "attack", "t": t, "campaign": "A4", "action": action,
                     "result": "n/a (no governor in the flat architecture)", "source": "qnx"}
                self.transcript[("A4", action)] = e
                self.write(e)
            return
        rc, out = ssh(self.slot, "slay -s KILL governord; sleep 1; grep -e 'governord restarted' "
                                 "/data/coldkeep/log/sup.log | tail -1")
        m = re.search(r"ready in ([\d.]+) ms", out)
        restart_ms = float(m.group(1)) if m else None
        # the restored governor must still apply G3 to a renewed 24 h defrost
        d = self.req({"op": "start_defrost", "case": 1, "seconds": 86400})
        t1 = self.status()["t"]
        self.record_decision(("A4", "start_defrost after restart", 1), d, t1)
        kill = {"type": "attack", "t": t, "campaign": "A4", "action": "kill governord",
                "result": f"restarted by sup, ready in {restart_ms} ms" if restart_ms is not None else "no restart seen",
                "restart_ms": restart_ms, "source": "qnx"}
        self.transcript[("A4", "kill governord")] = kill
        self.write(kill)
        # freeze: SIGSTOP for 5 s wall. casectl journals each G6 hold/release with the
        # sim time, so hold timing is read from the audit ring, not from shell polling.
        t2 = self.status()["t"]
        rc, out = ssh(self.slot, "slay -s STOP governord; sleep 5; slay -s CONT governord; sleep 1.5; "
                                 "grep -e 'G6 hold' -e 'G6 released' /dev/coldkeep/audit")
        holds, releases = {}, {}
        for line in out.splitlines():
            m = re.match(r"#\d+ t=(\d+) event case (\d+): G6 (hold|released)", line)
            if m and int(m.group(1)) >= t2:
                (holds if m.group(3) == "hold" else releases)[int(m.group(2))] = int(m.group(1))
        speed = self.args.speed
        hold_after = round((min(holds.values()) - t2) / speed, 2) if holds else None
        held_for = round(sum(releases[c] - holds[c] for c in holds if c in releases) / len(holds) / speed, 2) \
            if holds and all(c in releases for c in holds) else None
        freeze = {"type": "attack", "t": t2, "campaign": "A4", "action": "freeze governord 5 s",
                  "result": (f"{len(holds)} cases entered G6 hold (first ~{hold_after} s wall after the freeze), "
                             f"{len(releases)} released after resume; held ~{held_for} s"),
                  "hold_cases": len(holds), "released_cases": len(releases), "hold_after_s": hold_after,
                  "held_s": held_for, "source": "qnx"}
        self.transcript[("A4", "freeze governord 5 s")] = freeze
        self.write(freeze)
        self.a4_result = {"restart_ms": restart_ms, "hold_cases": len(holds), "released_cases": len(releases),
                          "hold_after_s": hold_after, "held_s": held_for, "after_restart_verdict": d.get("verdict"),
                          "after_restart_rule": d.get("rule")}

    def summary(self):
        per = {}
        prev = None
        for st in self.samples:
            for c in st["cases"]:
                p = per.setdefault(c["id"], {"id": c["id"], "type": c["type"], "limit": LIMIT[c["type"]],
                                             "peak_product": c["prod_true"], "peak_air": c["air_true"],
                                             "s_above": 0.0})
                p["peak_product"] = max(p["peak_product"], c["prod_true"])
                p["peak_air"] = max(p["peak_air"], c["air_true"])
                if prev is not None and c["prod_true"] > p["limit"]:
                    p["s_above"] += st["t"] - prev["t"]
            prev = st
        cases = []
        for p in per.values():
            p["minutes_above"] = round(p.pop("s_above") / 60.0, 2)
            cases.append(p)
        applied = sum(1 for d in self.decisions if d.get("verdict") in ("ALLOW", "CLAMP"))
        blocked = sum(1 for d in self.decisions if d.get("verdict") in ("DENY", "NEEDS_KEY"))
        self.result = {
            "type": "summary", "mode": self.mode,
            "t_start": self.samples[0]["t"], "t_end": self.samples[-1]["t"], "samples": len(self.samples),
            "minutes_above_total": round(sum(c["minutes_above"] for c in cases), 2),
            "food_at_risk_kg": sum(FOOD_KG[c["type"]] for c in cases if c["minutes_above"] > 0),
            "a1_applied": applied, "a1_blocked": blocked,
            "a3_succeeded": sum(1 for x in self.a3 if x["result"] == "SUCCEEDED"),
            "a3_attempts": sum(1 for x in self.a3 if not x["result"].startswith("n/a")),
            "max_jitter_us": max((c.get("jitter_us_max", 0) for st in self.samples for c in st["cases"]), default=0),
            "a2": self.a2_result,
            "a4": self.a4_result,
            "cases": cases,
        }
        self.write(self.result)


def git_sha():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True).stdout.strip()
    except OSError:
        return "unknown"


def describe(d):
    if not d:
        return "n/a"
    if "verdict" not in d:
        return d.get("result", "?")
    if d.get("rule") == "LEGACY":
        return "APPLIED" if d["verdict"] == "ALLOW" else "FAILED"
    return f'{d["verdict"]} ({d["rule"]} {d.get("reason", "")})' if d.get("rule") != "-" else d["verdict"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", default=datetime.datetime.now().strftime("run-%Y%m%d-%H%M%S"))
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--speed", type=float, default=30)
    ap.add_argument("--attack-at", type=float, default=600)
    ap.add_argument("--a2-at", type=float, default=1150)
    ap.add_argument("--a2-seconds", type=float, default=15)
    ap.add_argument("--a3-at", type=float, default=2100)
    ap.add_argument("--a4-at", type=float, default=2700)
    ap.add_argument("--end", type=float, default=3600)
    ap.add_argument("--legacy-slot", type=int, default=1)
    ap.add_argument("--protected-slot", type=int, default=0)
    ap.add_argument("--out", default=os.path.join(ROOT, "recordings"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    meta = {"label": a.label, "seed": a.seed, "speed": a.speed, "attack_at": a.attack_at, "a2_at": a.a2_at,
            "a2_seconds": a.a2_seconds, "a3_at": a.a3_at, "a4_at": a.a4_at,
            "end": a.end, "git": git_sha(), "date": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "source": "qnx-qemu",
            "note": "QNX 8.0 x86_64 in QEMU (emulated) on macOS; A3 runs shell commands with mgmtd's uid"}
    print(f"restarting both stores (seed {a.seed}, speed {a.speed:g}x)...")
    starts = [threading.Thread(target=ssh, args=(slot, f"sh /data/coldkeep/bin/coldkeep-start.sh {mode} "
                                                       f"{a.seed} {a.speed:g} > /dev/null"))
              for mode, slot in (("legacy", a.legacy_slot), ("protected", a.protected_slot))]
    for th in starts: th.start()
    for th in starts: th.join()

    stores = [Store("legacy", a.legacy_slot, a, os.path.join(a.out, f"{a.label}.legacy.jsonl"), meta),
              Store("protected", a.protected_slot, a, os.path.join(a.out, f"{a.label}.protected.jsonl"), meta)]
    threads = [threading.Thread(target=s.run) for s in stores]
    for th in threads: th.start()
    for th in threads: th.join()

    leg, pro = stores
    print()
    for key in sorted(set(leg.transcript) | set(pro.transcript), key=lambda k: (k[0], str(k[1:]))):
        name = " ".join(str(x) for x in key)
        print(f"[{name}] legacy: {describe(leg.transcript.get(key, {}))} | coldkeep: {describe(pro.transcript.get(key, {}))}")
    print(f"\nSummary ({a.label}; QNX 8.0 in QEMU; recordings in {os.path.relpath(a.out, ROOT)}/)")
    print(f"  {'store':10} {'min>limit':>10} {'peak product':>22} {'A1 applied':>11} {'A1 blocked':>11} "
          f"{'A3 succeeded':>13} {'food at risk':>13}")
    for s in stores:
        r = s.result
        worst = max(r["cases"], key=lambda c: c["peak_product"] - c["limit"])
        print(f"  {s.label:10} {r['minutes_above_total']:10.1f} {worst['peak_product']:>9.1f} C (case {worst['id']:>2})"
              f" {r['a1_applied']:11d} {r['a1_blocked']:11d} {r['a3_succeeded']:>8d}/{r['a3_attempts']}"
              f" {r['food_at_risk_kg']:10.0f} kg")


if __name__ == "__main__":
    sys.exit(main())
