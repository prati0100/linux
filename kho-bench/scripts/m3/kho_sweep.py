#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Resumable sweep driver for the KHO bench (one VM at a time).

M3 copy (kho-bench/scripts/m3/): all M3 modes, defaults m3_plan.tsv and
results/m3, optional per-point plan option bt=<s> (kernel B timeout).

Usage:
  kho_sweep.py [--plan scripts/m3/m3_plan.tsv] [--outdir results/m3]
               [--deadline-hours 8] [--max-extra-fails 2] [--dry-run]

Runs kho-bench/scripts/m3/run_point.sh (which boots the VM with `mk r`) one
run at a time until every plan point has enough rows in <outdir>/data.csv
(verify points: <outdir>/verify/data.csv).  State is recomputed from the
CSV before every run, so re-running the same command resumes the sweep.
Only rows with the current bzImage sha256 and b_cmdline_mode=nohp count.

Auto-cap rules (measured facts only, never extrapolation):
  1. generate_failed (ENOMEM in kernel A) at count N for a pattern ->
     skip every count >= N of that pattern (kernel A is the same image
     for both variants).
  2. b_ssh_timeout row, or an ok row with time_to_init > 600 s, at count
     N for (variant, pattern) -> that point is done and every larger count
     of the (variant, pattern) becomes kind=attempt target=1.  If this
     ends a point below its target, the reason logged in sweep.log (and
     printed by --dry-run) carries "WARNING: rule 2 truncated this point".
  3. points whose ok rows have median time_to_init > 60 s need only 3 ok
     runs (documented reduction for slow kernel-B boots).
  4. > max-extra-fails failed rows at a point -> GIVEUP for that point.
Stop gracefully with `touch <outdir>/STOP` (after the current run).
Progress: <outdir>/sweep.log (append) and <outdir>/sweep.status (one line).
"""
import argparse
import csv
import hashlib
import os
import signal
import statistics
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BENCH_DIR = os.path.dirname(os.path.dirname(SCRIPT_DIR))
TREE = os.path.dirname(BENCH_DIR)
VARIANTS = ["baseline", "early", "early_noinit1", "early_coalesce",
            "early_presize", "early_bulk", "early_nodefree",
            "early_batchinit", "early_opt"]
# plan variant keywords
VARIANT_SETS = {"both": ["baseline", "early"],
                "opt": [v for v in VARIANTS if v.startswith("early_")],
                "all": VARIANTS}
SLOW_TTI_S = 600.0
FEW_RUNS_TTI_S = 60.0
FEW_RUNS = 3

child = None


def utc():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


class Point:
    def __init__(self, idx, phase, variant, pattern, count, target, kind,
                 opts):
        self.idx, self.phase = idx, phase
        self.variant, self.pattern, self.count = variant, pattern, count
        self.target, self.kind = target, kind
        self.verify = "verify" in opts
        # bt=<s>: kernel B timeout for this point (env B_TIMEOUT of
        # run_point.sh); not a kho_bench param
        self.b_timeout = None
        for o in opts:
            if o.startswith("bt="):
                self.b_timeout = int(o[3:])
        self.params = [o for o in opts if "=" in o and
                       not o.startswith("bt=")]

    @property
    def params_str(self):
        return " ".join(self.params)

    def name(self):
        s = "%s %s %d" % (self.variant, self.pattern, self.count)
        if self.verify or self.params:
            s += " [%s]" % ",".join((["verify"] if self.verify else []) +
                                    self.params)
        return s


def load_plan(path):
    pts = []
    with open(path) as f:
        for ln in f:
            ln = ln.split("#", 1)[0].strip()
            if not ln:
                continue
            ph, var, pat, cnt, tgt, kind, opts = ln.split()
            assert kind in ("ok", "attempt"), ln
            opts = [] if opts == "-" else opts.split(",")
            for v in VARIANT_SETS.get(var, [var]):
                assert v in VARIANTS, ln
                pts.append(Point(len(pts), int(ph), v, pat, int(cnt),
                                 int(tgt), kind, opts))
    return pts


def read_rows(csvpath):
    if not os.path.exists(csvpath):
        return []
    with open(csvpath, newline="") as f:
        return list(csv.DictReader(f))


class State:
    """Per-point counts recomputed from the CSV files."""

    def __init__(self, outdir, sha):
        self.outdir, self.sha = outdir, sha

    def refresh(self):
        self.rows = {}
        for verify, path in ((False, os.path.join(self.outdir, "data.csv")),
                             (True, os.path.join(self.outdir, "verify",
                                                 "data.csv"))):
            for r in read_rows(path):
                if r.get("bzimage_sha") != self.sha or \
                        r.get("b_cmdline_mode") != "nohp":
                    continue
                key = (verify, r["variant"], r["pattern"], int(r["count"]),
                       r.get("params", ""))
                self.rows.setdefault(key, []).append(r)

    def rows_of(self, p):
        return self.rows.get((p.verify, p.variant, p.pattern, p.count,
                              p.params_str), [])

    @staticmethod
    def is_ok(p, r):
        return r["status"] == "ok" and (not p.verify or r["verify"] == "PASS")

    def counts(self, p):
        rows = self.rows_of(p)
        ok = sum(1 for r in rows if self.is_ok(p, r))
        return len(rows), ok

    def ok_ttis(self, p):
        return [float(r["time_to_init_s"]) for r in self.rows_of(p)
                if self.is_ok(p, r) and r["time_to_init_s"]]


def effective(p, pts, st, extra_fails, session_fails):
    """Return (target, kind, reason_if_done_or_skipped)."""
    target, kind = p.target, p.kind
    n, ok = st.counts(p)
    # rule 1: ENOMEM in A at a count <= this one (any variant, same pattern;
    # only TTI points, verify rows use fill=1 which changes nothing in A's
    # allocation pattern but keep them separate anyway)
    for q in pts:
        if q.pattern != p.pattern or q.count > p.count or q.verify != p.verify:
            continue
        if any("generate_failed" in r["status"] for r in st.rows_of(q)):
            if q is p:
                return target, kind, "generate_failed(ENOMEM) here"
            return target, kind, "SKIP generate_failed at %d" % q.count
    # rule 2: timeout / very slow at this point or a smaller count
    for q in pts:
        if q.variant != p.variant or q.pattern != p.pattern or \
                q.verify != p.verify or q.count > p.count:
            continue
        rows = st.rows_of(q)
        slow = any("b_ssh_timeout" in r["status"] for r in rows) or \
            any(t > SLOW_TTI_S for t in st.ok_ttis(q))
        if slow:
            if q is p:
                have = ok if kind == "ok" else n
                if have < target:
                    # rule 2 ends the point below its plan target: say so
                    # loudly in sweep.log / --dry-run instead of silently
                    return target, kind, (
                        "slow/timeout here (measured); WARNING: rule 2 "
                        "truncated this point at %d/%d %s rows" %
                        (have, target, kind))
                return target, kind, "slow/timeout here (measured)"
            target, kind = 1, "attempt"
    # rule 3
    ttis = st.ok_ttis(p)
    if ttis and statistics.median(ttis) > FEW_RUNS_TTI_S and kind == "ok":
        target = min(target, FEW_RUNS)
    # rule 4
    fails = n - ok + session_fails.get(p.idx, 0)
    have = ok if kind == "ok" else n
    if have < target and fails > extra_fails:
        return target, kind, "GIVEUP fails=%d" % fails
    return target, kind, ""


def need(p, pts, st, extra_fails, session_fails):
    target, kind, why = effective(p, pts, st, extra_fails, session_fails)
    if why:
        return 0, target, kind, why
    n, ok = st.counts(p)
    have = ok if kind == "ok" else n
    return max(0, target - have), target, kind, ""


class Logger:
    def __init__(self, outdir):
        self.log = os.path.join(outdir, "sweep.log")
        self.status = os.path.join(outdir, "sweep.status")

    def __call__(self, msg):
        line = "[%s] %s" % (utc(), msg)
        print(line, flush=True)
        with open(self.log, "a") as f:
            f.write(line + "\n")

    def set_status(self, msg):
        tmp = self.status + ".tmp"
        with open(tmp, "w") as f:
            f.write("[%s] %s\n" % (utc(), msg))
        os.replace(tmp, self.status)


def summary_counts(pts, st, extra_fails, session_fails):
    done = rem = 0
    boots = 0
    for p in pts:
        k = need(p, pts, st, extra_fails, session_fails)[0]
        boots += k
        if k:
            rem += 1
        else:
            done += 1
    return done, rem, boots


def run_point(p, outdir, log):
    global child
    cmd = [os.path.join(SCRIPT_DIR, "run_point.sh"), "--b-cmdline", "nohp",
           "--outdir", os.path.join(outdir, "verify") if p.verify else outdir]
    if p.verify:
        cmd.append("--verify")
    cmd += [p.variant, p.pattern, str(p.count), "1"] + p.params
    env = dict(os.environ)
    if p.b_timeout:
        env["B_TIMEOUT"] = str(p.b_timeout)
    t0 = time.time()
    with open(os.path.join(outdir, "sweep.out.run"), "a") as out:
        out.write("\n[%s] %s\n" % (utc(), " ".join(cmd)))
        out.flush()
        child = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, cwd=TREE,
                                 env=env)
        try:
            # run_point bounds itself (mk r timeout + ssh timeouts); this is
            # only a last-resort guard.  SIGTERM -> its trap stops our VM.
            rc = child.wait(timeout=7200)
        except subprocess.TimeoutExpired:
            log("ERROR run_point exceeded 7200 s; SIGTERM")
            child.terminate()
            rc = child.wait()
    child = None
    return rc, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", default=os.path.join(SCRIPT_DIR,
                                                   "m3_plan.tsv"))
    ap.add_argument("--outdir", default=os.path.join(BENCH_DIR, "results",
                                                     "m3"))
    ap.add_argument("--deadline-hours", type=float, default=8.0)
    ap.add_argument("--max-extra-fails", type=int, default=2)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    outdir = os.path.abspath(a.outdir)
    os.makedirs(outdir, exist_ok=True)
    bz = os.path.join(TREE, "arch/x86/boot/bzImage")
    if not os.path.exists(bz):
        sys.exit("no bzImage; build with mk first")
    sha = sha256(bz)
    pts = load_plan(a.plan)
    st = State(outdir, sha)
    session_fails = {}
    st.refresh()

    if a.dry_run:
        tot = 0
        for p in pts:
            k, target, kind, why = need(p, pts, st, a.max_extra_fails,
                                        session_fails)
            n, ok = st.counts(p)
            tot += k
            print("phase %d %-40s target=%d kind=%s rows=%d ok=%d need=%d %s"
                  % (p.phase, p.name(), target, kind, n, ok, k, why))
        print("remaining runs: %d (sha %s)" % (tot, sha[:12]))
        return 0

    import fcntl
    lockf = open("/tmp/kho-bench-sweep.lock", "a")
    try:
        fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit("another sweep holds /tmp/kho-bench-sweep.lock")

    def on_term(signum, frame):
        if child is not None:
            child.terminate()
            child.wait()
        sys.exit(128 + signum)
    signal.signal(signal.SIGTERM, on_term)

    log = Logger(outdir)
    deadline = time.time() + a.deadline_hours * 3600
    log("SWEEP START plan=%s outdir=%s sha=%s deadline=%.1fh" %
        (a.plan, outdir, sha, a.deadline_hours))
    reported = set()
    refusals = 0
    for phase in sorted({p.phase for p in pts}):
        ppts = [p for p in pts if p.phase == phase]
        rnd = 0
        while True:
            rnd += 1
            progressed = False
            for p in ppts:
                st.refresh()
                k, target, kind, why = need(p, pts, st, a.max_extra_fails,
                                            session_fails)
                if why and (p.idx, why) not in reported:
                    reported.add((p.idx, why))
                    log("%s phase=%d %s" % (
                        why if why.startswith(("SKIP", "GIVEUP"))
                        else "DONE(" + why + ")", phase, p.name()))
                n, ok = st.counts(p)
                have = ok if kind == "ok" else n
                # round-robin: at most `rnd` successful rows per point in
                # round `rnd`
                if k == 0 or have >= rnd:
                    continue
                if os.path.exists(os.path.join(outdir, "STOP")):
                    log("STOP file present; exiting")
                    return 0
                if time.time() > deadline:
                    log("deadline reached; exiting")
                    return 0
                if sha256(bz) != sha:
                    log("ERROR bzImage changed during sweep; exiting")
                    return 1
                done, rem, boots = summary_counts(pts, st, a.max_extra_fails,
                                                  session_fails)
                log("START phase=%d round=%d %s (%s %d/%d, rows=%d)" %
                    (phase, rnd, p.name(), kind, have + 1, target, n))
                log.set_status("RUNNING phase=%d %s | points done=%d "
                               "remaining=%d remaining_boots<=%d" %
                               (phase, p.name(), done, rem, boots))
                rc, wall = run_point(p, outdir, log)
                if rc == 3:
                    refusals += 1
                    log("REFUSED (VM busy) rc=3 #%d; sleeping 60 s" %
                        refusals)
                    if refusals >= 10:
                        log("ERROR 10 refusals; exiting")
                        return 1
                    time.sleep(60)
                    progressed = True
                    continue
                refusals = 0
                st.refresh()
                rows = st.rows_of(p)
                if len(rows) == n:
                    session_fails[p.idx] = session_fails.get(p.idx, 0) + 1
                    log("END %s rc=%d NO ROW WRITTEN wall=%.0fs" %
                        (p.name(), rc, wall))
                else:
                    r = rows[-1]
                    log("END %s rc=%d status=%s tti=%s verify=%s "
                        "walk1_ns=%s retrieve_ns=%s wall=%.0fs log=%s" %
                        (p.name(), rc, r["status"], r["time_to_init_s"] or
                         ("none(last_ts=%s)" % r.get("b_last_ts_s", "")),
                         r["verify"], r["walk1_ns"], r["retrieve_ns"], wall,
                         r["logfile"]))
                progressed = True
                done, rem, boots = summary_counts(pts, st, a.max_extra_fails,
                                                  session_fails)
                log("REMAINING points=%d boots<=%d (done points=%d)" %
                    (rem, boots, done))
                log.set_status("IDLE after %s | points done=%d remaining=%d "
                               "remaining_boots<=%d" %
                               (p.name(), done, rem, boots))
            if not progressed:
                break
        log("PHASE %d complete" % phase)
    st.refresh()
    done, rem, boots = summary_counts(pts, st, a.max_extra_fails,
                                      session_fails)
    log("SWEEP DONE points done=%d remaining=%d" % (done, rem))
    log.set_status("DONE points done=%d remaining=%d" % (done, rem))
    return 0


if __name__ == "__main__":
    sys.exit(main())
