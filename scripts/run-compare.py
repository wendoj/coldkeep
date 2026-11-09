#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the same scenario on a legacy and a protected COLDKEEP store on QEMU and
record both.

Both stores are restarted together (same seed, same speed). The runner records
telemetry, runs A1 (credentialed takeover through the documented API, paced at
one request per second) and A3 (the same privileged actions a compromised web
process would try, run as shell commands with mgmtd's uid on the target, so the
errors come from QNX itself). Summaries are computed only from recorded samples.

  scripts/run-compare.py [--label run1] [--seed 7] [--speed 30]
                         [--attack-at 600] [--a3-at 2100] [--end 3600]
"""
import argparse, datetime, json, os, socket, subprocess, sys, threading, time

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
        self.write({"type": "meta", **meta, "mode": mode, "slot": slot})

    def write(self, obj):
        self.f.write(json.dumps(obj) + "\n")
        self.f.flush()

    def connect(self):
        for _ in range(60):
            try:
                self.sock = socket.create_connection(("127.0.0.1", 18080 + self.slot), timeout=10)
                self.io = self.sock.makefile("rw")
                if self.req({"op": "login", "user": "admin", "pass": "admin"}).get("ok"):
                    return
            except OSError:
                time.sleep(0.5)
        raise RuntimeError(f"{self.mode}: cannot reach mgmtd")

    def req(self, obj):
        self.io.write(json.dumps(obj) + "\n")
        self.io.flush()
        return json.loads(self.io.readline())

    def status(self):
        return self.req({"op": "status"})

    def run(self):
        a = self.args
        self.connect()
        last_rec, attacked, a3_done = -1e9, False, False
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
            if not a3_done and t >= a.a3_at:
                a3_done = True
                self.a3_checks(t)
            if t >= a.end:
                break
            time.sleep(0.25)
        self.summary()
        self.f.close()

    def record_decision(self, key, r, t):
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

    def a3_checks(self, t):
        rc, out = ssh(self.slot, "pidin -f anU | grep bin/mgmtd")
        uid = out.split()[-1] if rc == 0 and out else "?"
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
    ap.add_argument("--a3-at", type=float, default=2100)
    ap.add_argument("--end", type=float, default=3600)
    ap.add_argument("--legacy-slot", type=int, default=1)
    ap.add_argument("--protected-slot", type=int, default=0)
    ap.add_argument("--out", default=os.path.join(ROOT, "recordings"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    meta = {"label": a.label, "seed": a.seed, "speed": a.speed, "attack_at": a.attack_at, "a3_at": a.a3_at,
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
