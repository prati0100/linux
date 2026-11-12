#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Generate summary.md / summary.csv for a KHO bench sweep from data.csv
and the raw serial logs.

Usage:
  kho_summary.py [--dir results/m2] [--sha SHA] [--out summary.md]
                 [--agg-csv summary.csv] [--plan scripts/m2_plan.tsv]
                 [--probe-dir results/probe] [--qemu-script ~/bin/qemu.sh]

Only measured rows are used (status=ok for statistics).  Timed-out points
are reported as lower bounds (TTI > B_TIMEOUT - 10 s of host wall time
after `kexec -e`, during which no init line appeared) and are never
extrapolated; nothing here fits curves.  A b_ssh_timeout run whose serial
log still contains B's init line is reported as a measured, flagged value
(columns tti_flagged_s / tti_flag), never as a lower bound.  Every row is
re-checked against its raw serial log (time_to_init_s re-parsed with
kho_parse.parse()).  The ENOMEM limits come from the M1 probe logs
(--probe-dir), parsed here from the kernel A serial log line
"generate pattern=P count=N failed after M blocks: -12".
"""
import argparse
import collections
import csv
import itertools
import os
import re
import statistics
import sys
import time

sys.dont_write_bytecode = True  # do not leave scripts/__pycache__ behind
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kho_parse  # noqa: E402
import kho_sweep  # noqa: E402  (auto-cap rule thresholds only)

PATTERNS = ["none", "contig", "alt", "random", "mixed", "inverted"]
VARIANTS = ["baseline", "early"]
# Timed restore phases of kernel B, per variant.  baseline's retrieve
# callback (kho_preserved_memory_reserve) does memblock_reserve() AND the
# struct page init (kho_get_preserved_page + page->private) per key, and
# baseline also runs kho_discover_noprsrv() (discover).  early splits the
# per-key work into walk1 (memblock_reserve only) and walk2 (struct page
# init), then memblock_free()s the radix node pages (free_nodes); it skips
# discover.  Baseline frees no radix node pages during boot.
PHASES = {"baseline": ["discover", "retrieve"],
          "early": ["walk1", "walk2", "free_nodes"]}
# a reserve-time ratio above SUPERQ x (count ratio)^2 is listed as faster
# than quadratic growth (10 % margin so run-to-run noise is not listed)
SUPERQ = 1.10
# a difference is called significant only if the exact MWU p is below this
ALPHA = 0.05
# count pair for the walk1-vs-retrieve growth comparison (reviewer-chosen
# ladder steps; the ratios themselves are computed from the data)
GROWTH_COUNTS = (262144, 1048576)
PROBE_RE = re.compile(r"generate pattern=(\S+) count=(\d+) failed after "
                      r"(\d+) blocks: (-?\d+)")
# VM configuration as seen by the kernel (serial log) and as configured in
# the qemu script that `mk r` runs (raw logs end with "~/bin/qemu.sh")
RE_SMP = re.compile(r"smp: Brought up (\d+) nodes?, (\d+) CPUs")
RE_MEMTOT = re.compile(r"\] Memory: \d+K/(\d+)K available")
RE_HYPER = re.compile(r"Hypervisor detected: (\S+)")
RE_DMI = re.compile(r"\] DMI: (?!Memory slots)(.*?)(?:, BIOS.*)?$")
RE_QEMU_OPT = re.compile(r"^\s*(-m|-smp|-cpu)\s+(\S+)")


def read(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def med(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def ms(ns):
    return None if ns is None else ns / 1e6


def fmt(x, nd=3):
    return "-" if x is None else ("%.*f" % (nd, x))


def fint(x):
    return "-" if x is None else "%d" % round(x)


def is_timeout(r):
    return "b_ssh_timeout" in r["status"]


def phase_sum_ms(r, variant):
    """Sum of the timed restore phases of one row (ms), None if none."""
    vals = [fnum(r.get(ph + "_ns")) for ph in PHASES.get(variant, [])]
    vals = [v for v in vals if v is not None]
    return ms(sum(vals)) if vals else None


def mwu_p(a, b):
    """Exact two-sided Mann-Whitney U p-value (full enumeration; ties
    count 1/2).  None if either sample has fewer than 2 values."""
    if len(a) < 2 or len(b) < 2:
        return None
    pool = list(a) + list(b)

    def u(xs, ys):
        return sum(1.0 if x > y else 0.5 if x == y else 0.0
                   for x in xs for y in ys)
    mid = len(a) * len(b) / 2.0
    obs = abs(u(a, b) - mid)
    hit = tot = 0
    for idx in itertools.combinations(range(len(pool)), len(a)):
        s = set(idx)
        xs = [pool[i] for i in idx]
        ys = [pool[i] for i in range(len(pool)) if i not in s]
        tot += 1
        if abs(u(xs, ys) - mid) >= obs - 1e-9:
            hit += 1
    return hit / tot


def separated(x, y):
    """True if the [min,max] TTI ranges of two points do not overlap."""
    return x["tti_max"] < y["tti_min"] or y["tti_max"] < x["tti_min"]


def is_sig(d):
    """Significant: exact MWU p < ALPHA.  Range separation alone is not
    enough (with 3 vs 3 runs the smallest attainable p is 0.100)."""
    return d["p"] is not None and d["p"] < ALPHA


def sig_label(d):
    """Text label of an early - baseline difference (dict from diffs)."""
    nb, ne = d["b"]["n_ok"], d["e"]["n_ok"]
    n = ("n=%d" % nb) if nb == ne else ("n=%d/%d" % (nb, ne))
    if is_sig(d) and d["sep"]:
        return "significant, %s, p=%s, ranges separated" % (n, fmt(d["p"]))
    if is_sig(d):
        return "p=%s < %.2f but ranges overlap, %s" % (fmt(d["p"]), ALPHA, n)
    if d["sep"]:
        return ("ranges separated, %s, p=%s, not significant at %.2f" %
                (n, fmt(d["p"]), ALPHA))
    return "within noise: ranges overlap, %s, p=%s" % (n, fmt(d["p"]))


def vm_config(logs, qemu_script):
    """VM configuration: values the kernel printed in the kernel B section
    (last occurrence) of every serial log, and the -m/-smp/-cpu options of
    the qemu script that `mk r` runs (read only)."""
    seen = collections.defaultdict(collections.Counter)
    for log in logs:
        last = {}
        tail = ""
        with open(log, errors="replace") as f:
            for line in f:
                if line.strip():
                    tail = line.strip()
                for k, rx in ((("smp", RE_SMP), ("mem", RE_MEMTOT),
                               ("hyper", RE_HYPER), ("dmi", RE_DMI))):
                    m = rx.search(line)
                    if m:
                        last[k] = m.groups()
        for k in ("smp", "mem", "hyper", "dmi"):
            seen[k][last.get(k)] += 1
        seen["tail"][(tail,)] += 1
    q = collections.OrderedDict()
    if qemu_script and os.path.exists(qemu_script):
        with open(qemu_script, errors="replace") as f:
            for line in f:
                m = RE_QEMU_OPT.match(line)
                if m and m.group(1) not in q:
                    q[m.group(1)] = m.group(2)
    return seen, q


def load_plan(path):
    """(variant, pattern, count) -> (target, kind, phase comment) for the
    TTI points of a kho_sweep plan (verify points skipped)."""
    plan = {}
    if not path or not os.path.exists(path):
        return plan
    comment = ""
    with open(path) as f:
        for line in f:
            s = line.strip()
            if s.startswith("# phase"):
                comment = s.lstrip("# ")
                continue
            if not s or s.startswith("#"):
                continue
            f_ = s.split()
            if len(f_) < 6 or "verify" in (f_[6] if len(f_) > 6 else ""):
                continue
            vs = VARIANTS if f_[1] == "both" else [f_[1]]
            for v in vs:
                plan[(v, f_[2], int(f_[3]))] = (int(f_[4]), f_[5], comment)
    return plan


def load_probes(pdir):
    """Rows of <pdir>/probe.csv, plus the ENOMEM details parsed from each
    generate_failed row's kernel A serial log."""
    out = []
    for r in read(os.path.join(pdir, "probe.csv")):
        e = dict(variant=r["variant"], pattern=r["pattern"],
                 count=int(r["count"]), status=r["status"],
                 log=os.path.join(os.path.basename(pdir), r["logfile"]),
                 build=r.get("kernel_build", ""),
                 gen_ms=fnum(r.get("gen_ms")), blocks=None, err=None)
        log = os.path.join(pdir, r["logfile"])
        if not e["build"] and os.path.exists(log):
            # probe.csv leaves kernel_build empty when generation failed
            # (no kernel B); take kernel A's "Linux version ... #N" line
            with open(log, errors="replace") as f:
                for line in f:
                    m = kho_parse.RE_LINUX.search(kho_parse.strip(line))
                    if m:
                        e["build"] = m.group(m.lastindex).strip()
                        break
        if "generate_failed" in r["status"] and os.path.exists(log):
            with open(log, errors="replace") as f:
                for line in f:
                    m = PROBE_RE.search(line)
                    if m and m.group(1) == r["pattern"] and \
                            int(m.group(2)) == e["count"]:
                        e["blocks"], e["err"] = int(m.group(3)), \
                            int(m.group(4))
                        break
        out.append(e)
    return out


def integrity(rows, base):
    """Re-parse each row's serial log; return list of problems."""
    probs = []
    for r in rows:
        log = os.path.join(base, r["logfile"])
        if not os.path.exists(log):
            probs.append("missing log %s" % r["logfile"])
            continue
        hlog = log[:-len(".log")] + ".harness.log"
        if not os.path.exists(hlog):
            probs.append("missing harness log for %s" % r["logfile"])
        p = kho_parse.parse(log)
        tti = p.get("time_to_init_s", "")
        if r["time_to_init_s"] and tti != r["time_to_init_s"]:
            probs.append("%s: csv tti %s != log %s" %
                         (r["logfile"], r["time_to_init_s"], tti))
        if r["status"] == "ok" and r.get("b_hugepages_1g_boot") != "0":
            probs.append("%s: B pre-allocated %s 1G pages" %
                         (r["logfile"], r.get("b_hugepages_1g_boot")))
        if r["status"] == "ok" and r.get("b_dmesg_init_s") and \
                abs(float(r["b_dmesg_init_s"]) - float(r["time_to_init_s"])) \
                > 0.001:
            probs.append("%s: dmesg init %s != serial %s" %
                         (r["logfile"], r["b_dmesg_init_s"],
                          r["time_to_init_s"]))
    return probs


def flagged_timers(r, base):
    """Timers of a flagged (b_ssh_timeout with init line) run, parsed from
    its serial log with kho_parse.parse(), plus the problems found when
    comparing them with the data.csv row."""
    p = kho_parse.parse(os.path.join(base, r["logfile"]))
    probs = []
    for k in ("memblock_reserved_regions", "discover_ns",
              "memblock_free_all_ns", "retrieve_ns", "walk1_ns"):
        if p.get(k, "") != r.get(k, ""):
            probs.append("%s: flagged run %s csv %r != log %r" %
                         (r["logfile"], k, r.get(k, ""), p.get(k, "")))
    return p, probs


def extra_per_node(r):
    """(regions added by walk1 - keys) / nodes for one early ok run, i.e.
    how many of the radix node pages ended up as separate reserved regions
    (None when not applicable)."""
    rc, rb, k, nd = (fnum(r.get(c)) for c in
                     ("memblock_reserved_regions", "reserved_before",
                      "keys", "nodes"))
    if None in (rc, rb, k, nd) or not nd:
        return None
    return (rc - rb - k) / nd


def aggregate(rows, base):
    pts = collections.OrderedDict()
    for r in rows:
        pts.setdefault((r["variant"], r["pattern"], int(r["count"])),
                       []).append(r)
    out = []
    fprobs = []
    for (v, p, n), rs in pts.items():
        ok = [r for r in rs if r["status"] == "ok"]
        tt = [float(r["time_to_init_s"]) for r in ok]
        to = [r for r in rs if is_timeout(r)]
        # timed out and no init line in the serial log: lower bound only
        to_noinit = [r for r in to if not r["time_to_init_s"]]
        # timed out over ssh, but the serial log has B's init line: a
        # measured (flagged) TTI, not a lower bound
        to_init = [r for r in to if r["time_to_init_s"]]
        fl = []
        for r in to_init:
            fp, pr = flagged_timers(r, base)
            fl.append(fp)
            fprobs += pr
        a = dict(variant=v, pattern=p, count=n, n_rows=len(rs),
                 n_ok=len(ok), n_timeout=len(to),
                 fail_status=";".join(sorted({r["status"] for r in rs
                                              if r["status"] != "ok"})),
                 tti_median=med(tt), tti_min=min(tt) if tt else None,
                 tti_max=max(tt) if tt else None,
                 tti_stdev=statistics.stdev(tt) if len(tt) > 1 else None,
                 tti_ok=tt,
                 keys=med([fnum(r["keys"]) for r in ok]),
                 nodes=med([fnum(r["nodes"]) for r in ok]),
                 walk1_ms=ms(med([fnum(r["walk1_ns"]) for r in ok])),
                 walk2_ms=ms(med([fnum(r["walk2_ns"]) for r in ok])),
                 free_nodes_ms=ms(med([fnum(r["free_nodes_ns"])
                                       for r in ok])),
                 discover_ms=ms(med([fnum(r["discover_ns"]) for r in ok])),
                 retrieve_ms=ms(med([fnum(r["retrieve_ns"]) for r in ok])),
                 # median over ok runs of the per-run sum of the variant's
                 # timed restore phases (PHASES)
                 restore_ms=med([phase_sum_ms(r, v) for r in ok]),
                 restore_phases="+".join(PHASES.get(v, [])),
                 mbfa_ms=ms(med([fnum(r["memblock_free_all_ns"])
                                 for r in ok])),
                 reserved_cnt=med([fnum(r["memblock_reserved_regions"])
                                   for r in ok]),
                 reserved_max=med([fnum(r["reserved_max"]) for r in ok]),
                 gen_ms=med([fnum(r.get("gen_ms")) for r in rs]),
                 # B never printed the init line although the host
                 # waited B_TIMEOUT s after `kexec -e` (and the serial log
                 # kept running until poweroff/kill).  10 s margin for the
                 # 1 s kexec delay and the kexec handoff.  Only for runs
                 # whose serial log has no init line.
                 timeout_lb_s=min([fnum(r["b_timeout_s"]) - 10
                                   for r in to_noinit])
                 if to_noinit else None,
                 last_printk_s=max([fnum(r["b_last_ts_s"]) or 0
                                    for r in to_noinit])
                 if to_noinit else None,
                 timeout_s=to[0]["b_timeout_s"] if to else "",
                 # B's init line can still appear in the serial log after
                 # the ssh window (qemu runs until poweroff/kill): that is
                 # a measured TTI of a run flagged b_ssh_timeout.
                 late_init_s=" ".join(r["time_to_init_s"] for r in to_init),
                 tti_flagged_s=" ".join(r["time_to_init_s"]
                                        for r in to_init),
                 tti_flag=("b_ssh_timeout: ssh not up within B_TIMEOUT=%s "
                           "s, serial-log init captured (measured, n=%d, "
                           "not in ok stats)" %
                           (to_init[0]["b_timeout_s"], len(to_init)))
                 if to_init else "",
                 flagged_restore_ms=" ".join(
                     fmt(phase_sum_ms(r, v), 6) for r in to_init),
                 flagged_reserve_ms=" ".join(
                     fmt(ms(fnum(r["walk1_ns"] if v != "baseline"
                                 else r["retrieve_ns"])), 6)
                     for r in to_init),
                 # parsed from the flagged runs' serial logs (kho_parse)
                 flagged_reserved_cnt=" ".join(
                     fp.get("memblock_reserved_regions", "") for fp in fl),
                 flagged_discover_ms=" ".join(
                     fmt(ms(fnum(fp.get("discover_ns"))), 6) for fp in fl),
                 flagged_mbfa_ms=" ".join(
                     fmt(ms(fnum(fp.get("memblock_free_all_ns"))), 6)
                     for fp in fl),
                 extra_per_node=med([extra_per_node(r) for r in ok]),
                 logs=" ".join(r["logfile"] for r in rs))
        # reserve-phase time: walk1 (early: memblock_reserve only) or
        # retrieve (baseline: memblock_reserve + struct page init).  Not
        # like-for-like between variants; compare restore_ms instead.
        a["reserve_ms"] = a["walk1_ms"] if v != "baseline" else \
            a["retrieve_ms"]
        out.append(a)
    order = {p: i for i, p in enumerate(PATTERNS)}
    out.sort(key=lambda a: (order.get(a["pattern"], 99), a["count"],
                            VARIANTS.index(a["variant"])
                            if a["variant"] in VARIANTS else 9))
    return out, fprobs


def slow_point(x):
    """kho_sweep.py rule 2 condition evaluated on one aggregated point."""
    return bool(x["n_timeout"]) or \
        any(t > kho_sweep.SLOW_TTI_S for t in x["tti_ok"])


def run_note(x, agg, plan, enomem):
    """Why a point has fewer than 5 runs: the plan target and the sweep's
    auto-cap rules (kho_sweep.py), evaluated on the measured rows."""
    if x["n_rows"] >= 5:
        return ""
    v, p, n = x["variant"], x["pattern"], x["count"]
    parts = []
    if x["n_rows"] == 1:
        parts.append("single attempt (n=1): one measurement, not a "
                     "statistic")
    pl = plan.get((v, p, n))
    if pl:
        parts.append("plan target %d (kind %s; m2_plan.tsv '%s')" % pl)
    smaller = [q for q in agg if q["variant"] == v and q["pattern"] == p
               and q["count"] < n and slow_point(q)]
    if smaller:
        parts.append("kho_sweep.py rule 2: TTI > %.0f s or ssh timeout "
                     "at count %d -> single attempt" %
                     (kho_sweep.SLOW_TTI_S, smaller[0]["count"]))
    elif slow_point(x):
        parts.append("kho_sweep.py rule 2: TTI > %.0f s or ssh timeout at "
                     "this point -> no further runs" % kho_sweep.SLOW_TTI_S)
    tts = x["tti_ok"] + [fnum(t) for t in x["tti_flagged_s"].split()]
    if tts and statistics.median(tts) > kho_sweep.FEW_RUNS_TTI_S and \
            not smaller and not slow_point(x):
        parts.append("kho_sweep.py rule 3: kernel B TTI > %.0f s -> %d "
                     "runs" % (kho_sweep.FEW_RUNS_TTI_S, kho_sweep.FEW_RUNS))
    elif pl and pl[0] < 5 and not smaller and not slow_point(x):
        e = enomem.get(p)
        parts.append("kernel B TTI is short (median %s s), so the reduced "
                     "count is the plan's choice, not rule 3; median "
                     "generation in kernel A here was %s s%s" %
                     (fmt(x["tti_median"]), fmt(x["gen_ms"] / 1e3
                                         if x["gen_ms"] is not None
                                         else None, 1),
                      (" (the plan expected slow generation from the M1 "
                       "probe at count %d: %s s on build %s; the M1 probe "
                       "hit ENOMEM after %d blocks)" %
                       (e["gen_probe"]["count"],
                        fmt(e["gen_probe"]["gen_ms"] / 1e3, 1),
                        e["gen_probe"]["build"].split()[0], e["blocks"]))
                      if e and e.get("gen_probe") else ""))
        gp = e.get("gen_probe") if e else None
        held = gp and x["gen_ms"] is not None and \
            x["gen_ms"] >= gp["gen_ms"]
        parts.append("documented deviation: %d runs fall short of the "
                     ">= 5-run rule for this point%s" %
                     (x["n_rows"],
                      "" if not gp or x["gen_ms"] is None else
                      (" and the plan's reason (slow generation) %s in M2 "
                       "(%s s measured vs %s s expected)" %
                       ("held" if held else "did not hold",
                        fmt(x["gen_ms"] / 1e3, 1),
                        fmt(gp["gen_ms"] / 1e3, 1)))))
    return "; ".join(parts)


def main():
    ap = argparse.ArgumentParser()
    sdir = os.path.dirname(os.path.abspath(__file__))
    here = os.path.dirname(sdir)
    ap.add_argument("--dir", default=os.path.join(here, "results", "m2"))
    ap.add_argument("--sha", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--agg-csv", default="")
    ap.add_argument("--plan", default=os.path.join(sdir, "m2_plan.tsv"))
    ap.add_argument("--probe-dir", default=os.path.join(here, "results",
                                                        "probe"))
    ap.add_argument("--qemu-script",
                    default=os.path.expanduser("~/bin/qemu.sh"),
                    help="qemu script run by `mk r` (read only, for the "
                    "VM configuration line)")
    a = ap.parse_args()
    base = os.path.abspath(a.dir)
    out_md = a.out or os.path.join(base, "summary.md")
    out_csv = a.agg_csv or os.path.join(base, "summary.csv")
    allrows = read(os.path.join(base, "data.csv"))
    vrows = read(os.path.join(base, "verify", "data.csv"))
    shas = collections.Counter(r.get("bzimage_sha", "") for r in allrows)
    sha = a.sha or (shas.most_common(1)[0][0] if shas else "")
    if not a.sha and len(shas) > 1:
        sys.exit("more than one bzImage sha in data.csv (%s); pass --sha" %
                 dict(shas))
    rows = [r for r in allrows if r.get("bzimage_sha") == sha and
            r.get("b_cmdline_mode") == "nohp" and r.get("params", "") == ""]
    excluded = len(allrows) - len(rows)
    probs = integrity(rows, base) + integrity(vrows, os.path.join(base,
                                                                  "verify"))
    agg, fprobs = aggregate(rows, base)
    probs += fprobs
    idx = {(x["variant"], x["pattern"], x["count"]): x for x in agg}
    plan = load_plan(a.plan)
    probes = load_probes(os.path.abspath(a.probe_dir))
    # smallest probe count per pattern that hit ENOMEM in kernel A, and the
    # largest successful probe of that pattern (for its generation time)
    enomem = {}
    for e in probes:
        if e["blocks"] is not None and (e["pattern"] not in enomem or
                                        e["count"] <
                                        enomem[e["pattern"]]["count"]):
            enomem[e["pattern"]] = dict(e)
    for pat, e in enomem.items():
        okp = [q for q in probes if q["pattern"] == pat and
               q["status"] == "ok" and q["gen_ms"] is not None]
        e["gen_probe"] = max(okp, key=lambda q: q["count"]) if okp else None
    for x in agg:
        x["run_note"] = run_note(x, agg, plan, enomem)

    cols = ["variant", "pattern", "count", "n_rows", "n_ok", "n_timeout",
            "tti_median", "tti_min", "tti_max", "tti_stdev", "keys", "nodes",
            "reserve_ms", "walk1_ms", "walk2_ms", "free_nodes_ms",
            "retrieve_ms", "mbfa_ms", "reserved_cnt", "reserved_max",
            "timeout_lb_s", "last_printk_s", "late_init_s", "timeout_s",
            "fail_status",
            # added in M2 iteration 2
            "discover_ms", "restore_ms", "restore_phases", "tti_flagged_s",
            "tti_flag", "flagged_reserve_ms", "flagged_restore_ms", "gen_ms",
            "run_note",
            # added in M2 iteration 3
            "flagged_reserved_cnt", "flagged_discover_ms", "flagged_mbfa_ms",
            "extra_per_node",
            "logs"]
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator="\n",
                           extrasaction="ignore")
        w.writeheader()
        for x in agg:
            w.writerow({k: ("" if x[k] is None else x[k]) for k in cols})

    L = []
    P = L.append
    P("# KHO early-reserve benchmark: M2 sweep summary (baseline vs early)")
    P("")
    P("Generated by `kho-bench/scripts/kho_summary.py` at %s from "
      "`data.csv` (%d rows used, %d excluded by filter), the raw serial "
      "logs in `raw/`, the sweep plan `%s` and the M1 probe logs in `%s`. "
      "Do not edit by hand." %
      (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), len(rows),
       excluded, os.path.relpath(a.plan, os.path.dirname(here)),
       os.path.relpath(os.path.abspath(a.probe_dir),
                       os.path.dirname(here))))
    P("")
    P("- Kernel image: bzImage sha256 `%s` (kernel build `%s`)." %
      (sha, rows[0]["kernel_build"] if rows else "?"))
    seen, qopt = vm_config([os.path.join(base, r["logfile"]) for r in rows],
                           a.qemu_script)

    def kv(cnt, f):
        return "; ".join("%s in %d logs" % (f(k), c) if k else
                         "not found in %d logs" % c
                         for k, c in cnt.most_common())
    P("- VM configuration. As printed by kernel B in its serial log (last "
      "occurrence per log, over the %d logs used): %s; %s; memory known to "
      "the kernel: %s; CPUs: %s. Last line of the raw logs (the qemu "
      "script that `mk r` runs): %s. That script is `%s`; its options now "
      "read: %s (read "
      "when this summary was generated, so it is only a cross-check of the "
      "logged values)." %
      (len(rows),
       kv(seen["dmi"], lambda k: k[0]),
       kv(seen["hyper"], lambda k: "hypervisor " + k[0]),
       kv(seen["mem"], lambda k: "%sK (%.1f GiB)" % (k[0],
                                                    int(k[0]) / 2.0 ** 20)),
       kv(seen["smp"], lambda k: "%s (%s NUMA node%s)" %
          (k[1], k[0], "" if k[0] == "1" else "s")),
       kv(seen["tail"], lambda k: "`%s`" % k[0]),
       a.qemu_script.replace(os.path.expanduser("~"), "~"),
       " ".join("`%s %s`" % kv_ for kv_ in qopt.items()) or "not found"))
    P("- Every number is a measured VM boot (`mk r` cold boot of kernel A, "
      "generation, kexec into kernel B with `kho_reserve_mode=<variant>`). "
      "Time to init (TTI) = printk timestamp of the last `Run /sbin/init as "
      "init process` in kernel B. Kernel B's cmdline has no hugepage "
      "parameters (b_cmdline_mode=nohp, B pre-allocates 0 1G pages).")
    P("- Statistics use rows with status=ok only. stdev is the sample "
      "standard deviation (`-` if fewer than 2 ok runs). Timer columns are "
      "medians over ok runs, in ms. reserved.cnt = memblock.reserved.cnt "
      "right after walk1 (early) or retrieve (baseline).")
    P("- Restore phases timed in kernel B (kernel/liveupdate/"
      "kexec_handover.c). **early** = walk1 (memblock_reserve() of every "
      "preserved range and radix node page; no struct page init) + walk2 "
      "(struct page init and page->private per key) + free_nodes "
      "(memblock_free() of the radix node pages). **baseline** = discover "
      "(kho_discover_noprsrv(); early skips it) + retrieve "
      "(kho_preserved_memory_reserve(): memblock_reserve() **and** struct "
      "page init per key, i.e. walk1 + walk2 work in one walk). Baseline "
      "frees no radix node pages during boot. So early walk1 alone is not "
      "comparable with baseline retrieve; the like-for-like comparison is "
      "**restore = walk1+walk2+free_nodes (early) vs discover+retrieve "
      "(baseline)**, shown as the median of the per-run sums. "
      "memblock_free_all is timed in both variants. In summary.csv, "
      "`reserve_ms` is walk1 (early) or retrieve (baseline), kept for "
      "compatibility only.")
    P("- Timed-out points without an init line did not reach init within "
      "B_TIMEOUT: the host waited B_TIMEOUT s of wall time after `kexec "
      "-e` and the serial log (captured until poweroff/kill) has no `Run "
      "/sbin/init` line for B. Their value is shown only as a lower bound "
      "`TTI > B_TIMEOUT - 10 s` (summary.csv `timeout_lb_s`; 10 s margin "
      "for the 1 s kexec delay and the handoff). A run flagged "
      "b_ssh_timeout whose serial log does contain B's init line is a "
      "measured TTI and is reported in `tti_flagged_s` with `tti_flag` "
      "(never as a lower bound, never in the ok statistics). Lower bounds "
      "are NOT measurements of TTI and are never extrapolated.")
    P("- Integrity: %d rows re-parsed from their serial logs; problems: %s."
      % (len(rows) + len(vrows), "none" if not probs else len(probs)))
    for pr in probs:
        P("  - %s" % pr)
    P("")

    P("## Run counts and exceptions")
    P("")
    P("Target: 5 ok runs per point. kho_sweep.py auto-cap rules (applied on "
      "measured rows only): rule 2, a b_ssh_timeout or an ok TTI > %.0f s "
      "at a count ends that point and turns every larger count of the "
      "same (variant, pattern) into a single attempt; rule 3, points whose "
      "ok median TTI > %.0f s need only %d ok runs. Points with fewer than "
      "5 runs:" % (kho_sweep.SLOW_TTI_S, kho_sweep.FEW_RUNS_TTI_S,
                   kho_sweep.FEW_RUNS))
    P("")
    P("| variant | pattern | count | ok/runs | reason |")
    P("|---|---|---:|---:|---|")
    for x in agg:
        if x["run_note"]:
            P("| %s | %s | %d | %d/%d | %s |" %
              (x["variant"], x["pattern"], x["count"], x["n_ok"],
               x["n_rows"], x["run_note"]))
    P("")

    P("## Results per pattern")
    for pat in PATTERNS:
        ps = [x for x in agg if x["pattern"] == pat]
        if not ps:
            continue
        P("")
        P("### %s" % pat)
        P("")
        P("| variant | count | ok/runs | TTI median s | min | max | stdev | "
          "walk1 ms | walk2 ms | free_nodes ms | discover ms | retrieve ms "
          "| restore ms | memblock_free_all ms | reserved.cnt | keys | "
          "notes |")
        P("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
          "---:|---:|---:|---:|---|")
        for x in ps:
            note = []
            if x["tti_flag"]:
                note.append("TTI %s s measured in the serial log, flagged "
                            "(%s); phase timers of that run, parsed from "
                            "its serial log (flagged, n=1, not in the ok "
                            "medians): reserve %s ms, restore %s ms, "
                            "discover %s ms, memblock_free_all %s ms, "
                            "reserved.cnt %s" %
                            (x["tti_flagged_s"], x["tti_flag"],
                             x["flagged_reserve_ms"],
                             x["flagged_restore_ms"],
                             x["flagged_discover_ms"],
                             x["flagged_mbfa_ms"],
                             x["flagged_reserved_cnt"]))
            if x["timeout_lb_s"] is not None:
                note.append("TIMEOUT: no init within B_TIMEOUT=%s s: TTI "
                            "> %s s (lower bound only; last printk at %s s)"
                            % (x["timeout_s"], fmt(x["timeout_lb_s"], 0),
                               fmt(x["last_printk_s"], 3)))
            if x["fail_status"] and not (x["tti_flag"] and
                                         x["fail_status"] == "b_ssh_timeout"):
                note.append("failed: " + x["fail_status"])
            if x["n_rows"] == 1:
                note.append("single attempt (n=1)")
            elif x["n_rows"] < 5:
                note.append("%d runs, see Run counts" % x["n_rows"])
            def cell(okval, flagged, nd=1):
                """ok median, or the flagged run's value marked as such"""
                if okval is not None or not x["tti_flag"] or \
                        flagged in ("", "-"):
                    return fmt(okval, nd) if nd is not None else fint(okval)
                return "%s (flagged)" % flagged
            P("| %s | %d | %d/%d | %s | %s | %s | %s | %s | %s | %s | %s | "
              "%s | %s | %s | %s | %s | %s |" %
              (x["variant"], x["count"], x["n_ok"], x["n_rows"],
               fmt(x["tti_median"]), fmt(x["tti_min"]), fmt(x["tti_max"]),
               fmt(x["tti_stdev"]), fmt(x["walk1_ms"], 1),
               fmt(x["walk2_ms"], 1), fmt(x["free_nodes_ms"], 1),
               cell(x["discover_ms"], fmt(fnum(x["flagged_discover_ms"]),
                                          1)),
               cell(x["retrieve_ms"], fmt(fnum(x["flagged_reserve_ms"]), 1)
                    if x["variant"] == "baseline" else ""),
               cell(x["restore_ms"], fmt(fnum(x["flagged_restore_ms"]), 1)),
               cell(x["mbfa_ms"], fmt(fnum(x["flagged_mbfa_ms"]), 1)),
               cell(x["reserved_cnt"], x["flagged_reserved_cnt"], None),
               fint(x["keys"]), "; ".join(note)))
    P("")

    # early - baseline
    P("## early minus baseline (median TTI and timed phases)")
    P("")
    P("restore = early walk1+walk2+free_nodes vs baseline discover+retrieve "
      "(median of per-run sums). residual = Δ TTI − Δ restore − Δ "
      "memblock_free_all: the part of the TTI difference not covered by "
      "any timer (medians of different quantities are subtracted, so a "
      "residual of a few ms is noise). sep = the [min,max] TTI ranges do "
      "not overlap. p = exact two-sided Mann-Whitney U (smallest attainable "
      "p: 2/C(n1+n2,n1), 0.008 for 5 vs 5 runs, 0.100 for 3 vs 3).")
    P("")
    P("| pattern | count | baseline median [min,max] (n) | early median "
      "[min,max] (n) | Δ TTI s | sep | p | early restore ms | baseline "
      "restore ms | Δ restore ms | Δ memblock_free_all ms | residual ms |")
    P("|---|---:|---|---|---:|---|---:|---:|---:|---:|---:|---:|")
    diffs = []
    for x in agg:
        if x["variant"] != "baseline" or not x["n_ok"]:
            continue
        e = idx.get(("early", x["pattern"], x["count"]))
        if not e or not e["n_ok"]:
            continue
        d = dict(pat=x["pattern"], n=x["count"], b=x, e=e,
                 dtti=e["tti_median"] - x["tti_median"],
                 sep=separated(x, e), p=mwu_p(x["tti_ok"], e["tti_ok"]))
        d["drest"] = (e["restore_ms"] - x["restore_ms"]) \
            if e["restore_ms"] is not None and x["restore_ms"] is not None \
            else None
        d["dmbfa"] = (e["mbfa_ms"] - x["mbfa_ms"]) \
            if e["mbfa_ms"] is not None and x["mbfa_ms"] is not None \
            else None
        d["resid"] = d["dtti"] * 1e3 - (d["drest"] or 0) - (d["dmbfa"] or 0)
        diffs.append(d)
        P("| %s | %d | %s [%s,%s] (%d) | %s [%s,%s] (%d) | %+.3f | %s | %s "
          "| %s | %s | %s | %s | %s |" %
          (d["pat"], d["n"], fmt(x["tti_median"]), fmt(x["tti_min"]),
           fmt(x["tti_max"]), x["n_ok"], fmt(e["tti_median"]),
           fmt(e["tti_min"]), fmt(e["tti_max"]), e["n_ok"], d["dtti"],
           "yes" if d["sep"] else "no", fmt(d["p"]),
           fmt(e["restore_ms"], 1), fmt(x["restore_ms"], 1),
           "-" if d["drest"] is None else "%+.1f" % d["drest"],
           "-" if d["dmbfa"] is None else "%+.1f" % d["dmbfa"],
           "%+.1f" % d["resid"]))
    P("")
    sig = [d for d in diffs if is_sig(d)]
    sep_only = [d for d in diffs if d["sep"] and not is_sig(d)]
    p_only = [d for d in diffs if is_sig(d) and not d["sep"]]
    P("- Significant (exact MWU p < %.2f): %s. All of them also have "
      "separated [min,max] ranges." %
      (ALPHA, ", ".join("%s %d (%+.3f s, p=%s)" %
                        (d["pat"], d["n"], d["dtti"], fmt(d["p"]))
                        for d in sig if d["sep"]) or "none"))
    if p_only:
        P("- p < %.2f but [min,max] ranges overlap: %s." %
          (ALPHA, ", ".join("%s %d (%+.3f s, p=%s)" %
                            (d["pat"], d["n"], d["dtti"], fmt(d["p"]))
                            for d in p_only)))
    for d in sep_only:
        x, e = d["b"], d["e"]
        few = [q for q in (x, e) if q["n_rows"] < 5]
        why = ""
        if few:
            gp = enomem.get(d["pat"], {}).get("gen_probe")
            why = (" %s runs fall short of the >= 5-run target; kho_sweep.py "
                   "rule 3 (only %d runs if median TTI > %.0f s) does not "
                   "apply (median TTI %s / %s s), so this is a documented "
                   "deviation (see Run counts)." %
                   (" vs ".join(str(q["n_rows"]) for q in (x, e)),
                    kho_sweep.FEW_RUNS, kho_sweep.FEW_RUNS_TTI_S,
                    fmt(x["tti_median"]), fmt(e["tti_median"])))
            gen = [q["gen_ms"] for q in (x, e) if q["gen_ms"] is not None]
            if gp and gen:
                why += (" The plan's reason for fewer runs, slow generation "
                        "in kernel A expected from the M1 probe (%s s at "
                        "count %d, build %s), did not hold in M2: measured "
                        "median generation at this point was %s s." %
                        (fmt(gp["gen_ms"] / 1e3, 1), gp["count"],
                         gp["build"].split()[0],
                         fmt(statistics.median(gen) / 1e3, 1)))
        P("- %s %d (%+.3f s): %s. With %d vs %d runs the smallest "
          "attainable exact p is %s, so this point cannot reach p < %.2f; "
          "it is not called significant.%s" %
          (d["pat"], d["n"], d["dtti"], sig_label(d), x["n_ok"], e["n_ok"],
           fmt(2.0 / len(list(itertools.combinations(
               range(x["n_ok"] + e["n_ok"]), x["n_ok"])))), ALPHA, why))
    P("- Every other early − baseline difference has overlapping ranges "
      "and p >= %.2f, i.e. it is within run-to-run noise." % ALPHA)
    small = [d for d in sig if d["sep"] and d["n"] < 262144]
    if small:
        P("- Below 262144 keys the significant (p < %.2f, ranges "
          "separated) differences are: "
          "%s. They are measured early-mode costs (early free_nodes there: "
          "%s)." %
          (ALPHA, ", ".join("%s %d (%+.0f ms)" % (d["pat"], d["n"],
                                                  d["dtti"] * 1e3)
                            for d in small),
           ", ".join("%s %d %s ms" % (d["pat"], d["n"],
                                       fmt(d["e"]["free_nodes_ms"], 1))
                     for d in small)))
    P("")
    P("Phase decomposition at the largest count measured for both variants "
      "(per pattern). The term walk1+walk2 − retrieve is not the same work "
      "on both sides: early walk1 also memblock_reserve()s every radix node "
      "page through the walk's .node callback (kho_early_reserve_node, "
      "called at kernel/liveupdate/kexec_handover.c:451-455 for leaf "
      "bitmaps and :479-483 for interior nodes), and baseline retrieve "
      "walks with a .leaf callback only, so it reserves no node pages "
      "(mechanism item (c) below). The node-page part is not separately "
      "timed:")
    P("")
    for pat in PATTERNS[1:]:
        dp = [d for d in diffs if d["pat"] == pat]
        if not dp:
            continue
        d = max(dp, key=lambda d: d["n"])
        b, e = d["b"], d["e"]
        P("- **%s %d** (%s): Δ TTI %+.0f ms. early walk1 %s + walk2 %s + "
          "free_nodes %s = %s ms; baseline discover %s + retrieve %s = %s "
          "ms; Δ restore %s ms = discover (baseline only) %s ms + "
          "free_nodes (early only) +%s ms + (walk1+walk2 − retrieve: "
          "per-key reserve + struct page init split over two walks, plus "
          "early-only radix node-page reserves (c), %s nodes; not "
          "separately timed) %s ms; Δ memblock_free_all %s ms; residual not "
          "covered by any timer %+.0f ms." %
          (pat, d["n"], sig_label(d),
           d["dtti"] * 1e3, fmt(e["walk1_ms"], 1), fmt(e["walk2_ms"], 1),
           fmt(e["free_nodes_ms"], 1), fmt(e["restore_ms"], 1),
           fmt(b["discover_ms"], 1), fmt(b["retrieve_ms"], 1),
           fmt(b["restore_ms"], 1),
           "-" if d["drest"] is None else "%+.0f" % d["drest"],
           "-" if b["discover_ms"] is None else "%+.0f" % -b["discover_ms"],
           fmt(e["free_nodes_ms"], 0), fint(e["nodes"]),
           "-" if None in (e["walk1_ms"], e["walk2_ms"], b["retrieve_ms"])
           else "%+.0f" % (e["walk1_ms"] + e["walk2_ms"] - b["retrieve_ms"]),
           "-" if d["dmbfa"] is None else "%+.1f" % d["dmbfa"], d["resid"]))
    P("")
    ga, gb = GROWTH_COUNTS
    growth = []
    for pat in PATTERNS[1:]:
        q = [idx.get((v, pat, n)) for v in VARIANTS for n in (ga, gb)]
        if not all(r and r["n_ok"] for r in q):
            continue
        b0, b1, e0, e1 = q
        if None in (b0["retrieve_ms"], b1["retrieve_ms"], e0["walk1_ms"],
                    e1["walk1_ms"]):
            continue
        growth.append("%s early walk1 x%.2f (%s -> %s ms, nodes %s -> %s) "
                      "vs baseline retrieve x%.2f (%s -> %s ms)" %
                      (pat, e1["walk1_ms"] / e0["walk1_ms"],
                       fmt(e0["walk1_ms"], 1), fmt(e1["walk1_ms"], 1),
                       fint(e0["nodes"]), fint(e1["nodes"]),
                       b1["retrieve_ms"] / b0["retrieve_ms"],
                       fmt(b0["retrieve_ms"], 1), fmt(b1["retrieve_ms"], 1)))
    if growth:
        P("Measured growth of the reserve phase from %d to %d keys (count "
          "x%.2f; medians of ok runs; early walk1 includes the node-page "
          "reserves, baseline retrieve includes struct page init but no "
          "node pages): %s. Where walk1 grows faster than retrieve, the "
          "early walk1 work per key is not the same as baseline's." %
          (ga, gb, gb / ga, "; ".join(growth)))
    P("")
    P("The residual is measured as a difference, not attributed: no timer "
      "covers it. A plausible but untested cause is that in early mode the "
      "memblock allocations made between walk1 (in setup_arch) and "
      "memblock_free_all scan a reserved array that already holds all N "
      "preserved regions, while in baseline the array only grows at "
      "retrieve.")
    P("")
    # points where at least one variant has only single attempts or
    # flagged runs: list, do not compare
    single = []
    for x in agg:
        if x["variant"] != "baseline":
            continue
        e = idx.get(("early", x["pattern"], x["count"]))
        if not e:
            continue
        if (x["n_rows"] == 1 or e["n_rows"] == 1) and \
                (x["n_ok"] or x["tti_flagged_s"]) and \
                (e["n_ok"] or e["tti_flagged_s"]):
            single.append((x, e))
    for x, e in single:
        ref = [d for d in diffs if d["pat"] == x["pattern"] and
               d["n"] < x["count"] and d["b"]["n_ok"] >= 3 and
               d["e"]["n_ok"] >= 3]
        spread = ""
        if ref:
            d = max(ref, key=lambda d: d["n"])
            spread = (" At %d, the largest count with >= 3 runs per "
                      "variant, the run-to-run spread (max − min) / median "
                      "is baseline %.1f%%, early %.1f%%, and early − "
                      "baseline (%+.3f s) is %s." %
                      (d["n"], 100 * (d["b"]["tti_max"] - d["b"]["tti_min"])
                       / d["b"]["tti_median"],
                       100 * (d["e"]["tti_max"] - d["e"]["tti_min"]) /
                       d["e"]["tti_median"], d["dtti"], sig_label(d)))

        def val(q):
            if q["n_ok"]:
                return "%s s (status ok, n=%d)" % (fmt(q["tti_median"]),
                                                   q["n_ok"])
            return "%s s (n=%d, %s)" % (q["tti_flagged_s"],
                                        len(q["tti_flagged_s"].split()),
                                        q["tti_flag"])
        P("- **%s %d: single attempts, not compared.** baseline %s; early "
          "%s.%s With one run per variant there is no spread estimate at "
          "this count (and the baseline run is %s), so no early − baseline "
          "difference is claimed here." %
          (x["pattern"], x["count"], val(x), val(e), spread,
           "flagged" if x["tti_flag"] else "not flagged"))
    P("")

    # worst case
    P("## Worst case (measured data only)")
    P("")
    P("Worst of the tested patterns (%s) at the measured counts. This is "
      "not a proven upper bound: untested layouts with more inverted "
      "orders (more blocks walked before blocks at lower addresses) could "
      "cost more per insert; they were not measured and no number is "
      "given for them." % ", ".join(PATTERNS[1:]))
    P("")
    okpts = [x for x in agg if x["n_ok"] and x["pattern"] != "none"]
    stat = [x for x in okpts if x["n_ok"] >= 3]
    worst_pat = None
    for v in VARIANTS:
        vp = [x for x in stat if x["variant"] == v]
        if not vp:
            continue
        w = max(vp, key=lambda x: x["tti_median"])
        worst_pat = w["pattern"]
        P("- **%s**, largest median TTI of the tested patterns over points "
          "with >= 3 ok runs: "
          "pattern **%s** at count %d: %s s (n=%d, min %s, max %s); restore "
          "%s ms (%s), reserved.cnt %s." %
          (v, w["pattern"], w["count"], fmt(w["tti_median"]), w["n_ok"],
           fmt(w["tti_min"]), fmt(w["tti_max"]), fmt(w["restore_ms"], 1),
           w["restore_phases"], fint(w["reserved_cnt"])))
    if worst_pat:
        top = [x for x in agg if x["pattern"] == worst_pat and
               (x["n_ok"] or x["tti_flagged_s"])]
        nmax = max(x["count"] for x in top)
        vals = []
        for x in top:
            if x["count"] != nmax:
                continue
            vals += [(float(t), x["variant"], "ok")
                     for t in x["tti_ok"]]
            vals += [(float(t), x["variant"], "flagged b_ssh_timeout")
                     for t in x["tti_flagged_s"].split()]
        nper = collections.Counter(v for _, v, _ in vals)
        P("- Largest measured count of the worst tested pattern: **%s %d: "
          "%s–%s s** "
          "(%s).%s" %
          (worst_pat, nmax, fmt(min(vals)[0], 0), fmt(max(vals)[0], 0),
           "; ".join("%s %.3f s %s" % (v, t, s) for t, v, s in
                     sorted(vals, key=lambda z: z[1])),
           " These are single attempts (n=1 per variant), not statistics; "
           "the two variants are not distinguishable at this count (see "
           "above)." if max(nper.values()) == 1 else ""))
    P("")
    P("Largest swept count per pattern (log ladder of the plan; this is not "
      "the memory limit). The memory limit is where kernel A's generator "
      "hit ENOMEM in the M1 probes; those probe counts were not re-swept "
      "here:")
    P("")
    blds = sorted({e["build"].split()[0] for e in enomem.values()
                   if e["build"]})
    P("| pattern | variant | largest swept count with ok runs | TTI median "
      "s | restore ms | reserved.cnt | larger counts swept | M1 probe ENOMEM "
      "in kernel A, build %s (count tried: blocks generated before -12; "
      "log) |" % ("/".join(blds) or "?"))
    P("|---|---|---:|---:|---:|---:|---|---|")
    for pat in PATTERNS[1:]:
        e = enomem.get(pat)
        oth = [q for q in probes if q["pattern"] == pat and
               q["status"] != "ok" and q["blocks"] is None]
        if e:
            pr = "%d: %d (build %s, %s)" % (e["count"], e["blocks"],
                                            e["build"].split()[0],
                                            e["log"])
        elif oth:
            pr = "none; probe " + "; ".join(
                "%s %d: %s (build %s, %s)" % (q["variant"], q["count"],
                                              q["status"],
                                              q["build"].split()[0],
                                              q["log"]) for q in oth)
        else:
            pr = "none"
        for v in VARIANTS:
            vp = [x for x in agg if x["variant"] == v and x["pattern"] == pat]
            okp = [x for x in vp if x["n_ok"]]
            if not okp:
                continue
            m = max(okp, key=lambda x: x["count"])
            larger = ["%d: %s" % (x["count"],
                                  ("init at %s s in serial log, %s" %
                                   (x["tti_flagged_s"], x["tti_flag"]))
                                  if x["tti_flagged_s"] else
                                  ("timeout: TTI > %s s" %
                                   fmt(x["timeout_lb_s"], 0))
                                  if x["timeout_lb_s"] is not None else
                                  (x["fail_status"] or "?"))
                      for x in vp if x["count"] > m["count"]]
            P("| %s | %s | %d | %s | %s | %s | %s | %s |" %
              (pat, v, m["count"], fmt(m["tti_median"]),
               fmt(m["restore_ms"], 1), fint(m["reserved_cnt"]),
               "; ".join(larger) or "-", pr))
    P("")
    for pat in PATTERNS[1:]:
        e = enomem.get(pat)
        sw = [x["count"] for x in agg if x["pattern"] == pat and
              (x["n_ok"] or x["tti_flagged_s"])]
        if not sw:
            continue
        if e:
            pb = sorted(q["count"] for q in probes if q["pattern"] == pat
                        and q["status"] == "ok" and q["count"] > max(sw))
            P("- %s: generation in kernel A is memory-bound. Largest swept "
              "count %d; the M1 probe (build %s) at %d failed in kernel A "
              "with -12 after %d blocks. That only shows kernel A could "
              "generate up to %d blocks; %s." %
              (pat, max(sw), e["build"].split()[0], e["count"],
               e["blocks"], e["blocks"],
               ("kernel B was booted above %d only in M1 probes at %s "
                "(single runs, not in this sweep)" %
                (max(sw), ", ".join(str(c) for c in pb))) if pb else
               ("kernel B was never booted at counts above %d (neither in "
                "this sweep nor in the M1 probes), so its restore there is "
                "not measured" % max(sw))))
        else:
            big = [x for x in agg if x["pattern"] == pat and
                   x["count"] == max(sw)]
            tt = [t for x in big for t in
                  x["tti_ok"] + [float(s) for s in
                                 x["tti_flagged_s"].split()]]
            nrun = ", ".join("%s n=%d" % (x["variant"], x["n_rows"])
                             for x in big)
            gen = [x["gen_ms"] for x in big if x["gen_ms"] is not None]
            P("- %s: time-bound, not memory-bound. No ENOMEM in any probe "
              "or sweep row; at %d kernel A generated the preservations in "
              "%s s, but kernel B needed %s–%s s to reach init (%s).%s" %
              (pat, max(sw), fmt(min(gen) / 1e3, 1) if gen else "-",
               fmt(min(tt), 0), fmt(max(tt), 0), nrun,
               "" if any(x["pattern"] == pat and x["count"] > max(sw)
                         for x in agg) else
               " No larger count was attempted."))
    P("")
    P("Pattern with the largest median TTI at each count measured for all "
      "five patterns (per variant). The worst label is marked "
      "**separated** only if its [min,max] range lies above the range of "
      "every other pattern; otherwise it is within noise:")
    P("")
    counts = sorted({x["count"] for x in okpts})
    tables = [("five", PATTERNS[1:]),
              ("four", [p for p in PATTERNS[1:] if p != "mixed"])]
    clear_from = {}
    for name, pats in tables:
        if name == "four":
            P("Pattern with the largest median TTI at each count measured "
              "for all patterns except mixed (mixed is memory-limited, see "
              "above):")
            P("")
        P("| count | variant | " + " | ".join(pats) + " | worst |")
        P("|---:|---|" + "---:|" * len(pats) + "---|")
        hist = collections.defaultdict(list)
        for n in counts:
            for v in VARIANTS:
                row = [idx.get((v, p, n)) for p in pats]
                if not all(r and r["n_ok"] for r in row):
                    continue
                w = max(row, key=lambda r: r["tti_median"])
                clear = all(w["tti_min"] > r["tti_max"] for r in row
                            if r is not w)
                hist[v].append((n, w["pattern"], clear))
                P("| %d | %s | %s | %s (%s) |" %
                  (n, v, " | ".join(fmt(r["tti_median"]) for r in row),
                   w["pattern"], "separated" if clear else "within noise"))
        P("")
        for v in VARIANTS:
            h = hist.get(v, [])
            # smallest count from which the same pattern is the separated
            # worst at every larger count of this table
            start = None
            for i in range(len(h)):
                tail = h[i:]
                if all(c and pn == tail[-1][1] for _, pn, c in tail):
                    start = h[i]
                    break
            if start:
                clear_from.setdefault((start[1], v), []).append(
                    (name, start[0], h[-1][0]))
    for (pn, v), lst in sorted(clear_from.items()):
        P("- %s: **%s** is clearly the worst pattern (range separated from "
          "every other pattern) %s." %
          (v, pn, "; ".join("from count %d up to %d in the %s-pattern table"
                            % (s, e, nm) for nm, s, e in lst)))
    P("- At smaller counts the worst-pattern label only picks the largest "
      "median and is within noise unless marked separated.")
    P("")
    to = [x for x in agg if x["n_timeout"]]
    if to:
        P("Runs flagged b_ssh_timeout (B not reachable over ssh within "
          "B_TIMEOUT; lower bound unless an init line was captured):")
        P("")
        for x in to:
            if x["tti_flagged_s"]:
                P("- %s %s %d: B was not reachable over ssh within "
                  "B_TIMEOUT=%s s, but its serial log shows `Run /sbin/init` "
                  "at %s s (a measured TTI; the run is flagged b_ssh_timeout "
                  "and excluded from the ok statistics; summary.csv: "
                  "tti_flagged_s, timeout_lb_s empty); logs: %s" %
                  (x["variant"], x["pattern"], x["count"], x["timeout_s"],
                   x["tti_flagged_s"], x["logs"]))
            if x["timeout_lb_s"] is not None:
                P("- %s %s %d: did not reach init within B_TIMEOUT=%s s, "
                  "i.e. TTI > %s s (lower bound, not a measurement; last "
                  "kernel-B printk at %s s); logs: %s" %
                  (x["variant"], x["pattern"], x["count"], x["timeout_s"],
                   fmt(x["timeout_lb_s"], 0), fmt(x["last_printk_s"], 3),
                   x["logs"]))
        P("")

    # scaling evidence: measured ratios between consecutive measured counts
    P("## Why: reserve-phase scaling between measured counts")
    P("")
    P("Ratio of the median reserve-phase time between consecutive measured "
      "counts, next to the count ratio. The reserve phase is walk1 for "
      "early (memblock_reserve only) and retrieve for baseline "
      "(memblock_reserve + struct page init); compare each variant with "
      "itself only. A ratio near the count ratio means linear cost per "
      "key; near its square means quadratic. These ratios are computed "
      "between two measured points only. A point with a single run or a "
      "flagged run is marked in the last column.")
    P("")
    P("| pattern | variant | count a -> b | count ratio | reserve ms a -> b "
      "| time ratio | reserved.cnt b | TTI b s | point b |")
    P("|---|---|---|---:|---|---:|---:|---:|---|")
    superq = []
    for pat in PATTERNS[1:]:
        for v in VARIANTS:
            vp = []
            for x in agg:
                if x["variant"] != v or x["pattern"] != pat or \
                        x["count"] < 32768:
                    continue
                if x["n_ok"] and x["reserve_ms"]:
                    vp.append((x, x["reserve_ms"], x["tti_median"],
                               "n=%d ok" % x["n_ok"] if x["n_ok"] < 5
                               else ""))
                elif x["flagged_reserve_ms"]:
                    rv = fnum(x["flagged_reserve_ms"].split()[0])
                    vp.append((x, rv, fnum(x["tti_flagged_s"].split()[0]),
                               "n=1, flagged b_ssh_timeout (timers from "
                               "its serial log)"))
            for (x0, r0, _, _), (x1, r1, t1, nb) in zip(vp, vp[1:]):
                cr, tr = x1["count"] / x0["count"], r1 / r0
                if tr > SUPERQ * cr * cr:
                    superq.append("%s %s %d->%d x%.2f (count x%.2f, square "
                                  "%.2f%s)" % (pat, v, x0["count"],
                                               x1["count"], tr, cr, cr * cr,
                                               "; " + nb if nb else ""))
                P("| %s | %s | %d -> %d | %.2f | %s -> %s | %.2f | %s | %s "
                  "| %s |" % (pat, v, x0["count"], x1["count"], cr,
                              fmt(r0, 1), fmt(r1, 1), tr,
                              fint(x1["reserved_cnt"]) if x1["n_ok"] else
                              "-", fmt(t1), nb or "-"))
    P("")
    if superq:
        P("Steps where the measured time ratio exceeds the square of the "
          "count ratio by more than %.0f%% (faster than quadratic growth): "
          "%s. These are "
          "measured as listed; the steps at the largest counts rest on "
          "single runs. They are not explained by the code model below "
          "(which predicts quadratic); a cache effect from the memmove "
          "working set (N x sizeof(struct memblock_region)) is an untested "
          "hypothesis. No curve is fitted and nothing is extrapolated." %
          ((SUPERQ - 1) * 100, "; ".join(superq)))
        P("")
    dc = [d for d in diffs if d["pat"] == "contig" and
          d["b"]["discover_ms"] is not None]
    dc = max(dc, key=lambda d: d["n"]) if dc else None
    slower = collections.OrderedDict()
    for d in diffs:
        if is_sig(d) and d["sep"] and d["dtti"] > 0:
            slower.setdefault(d["pat"], []).append(d["n"])
    # (walk1 regions added - keys) / nodes for early at the decomposition
    # count of each pattern (median over ok runs)
    epn = []
    for pat in PATTERNS[1:]:
        dp = [d for d in diffs if d["pat"] == pat]
        if dp:
            e = max(dp, key=lambda d: d["n"])["e"]
            if e["extra_per_node"] is not None:
                epn.append((pat, e))
    epn_pos = [e["extra_per_node"] for _, e in epn
               if e["extra_per_node"] > 0]
    P("Mechanism (from the code, mm/memblock.c and "
      "kernel/liveupdate/kexec_handover.c): both variants call "
      "memblock_reserve() once per radix key, in radix-walk order. "
      "memblock_add_range() finds the slot with a binary search "
      "(__memblock_search) and then memblock_insert_region() memmove()s "
      "every region above the slot up by one. The KHO radix key puts the "
      "order bit above the shifted PFN (kho_encode_radix_key: higher order "
      "-> smaller key), so the walk visits all keys of one order in "
      "ascending address order, then the next lower order. Consequences: "
      "contig merges into a few regions (reserved.cnt stays small); alt and "
      "random (order 0 only, ascending) append at the end of the array "
      "(memmove of 0 regions, linear total); inverted places its order-1 "
      "blocks above its order-0 blocks, so the order-1 blocks are walked "
      "first and every later order-0 insert lands below all ~N/2 order-1 "
      "regions, costing a memmove of ~N/2 regions per insert (quadratic "
      "total); mixed interleaves 11 orders and is quadratic in the same "
      "way, but generation in kernel A runs out of memory (M1 probe "
      "above). The early variant does the same per-key inserts in walk1 "
      "(plus the node-page inserts of (c)), so it has the same order "
      "dependence as baseline's retrieve. "
      "Differences between the variants (see the decomposition above): "
      "(a) baseline runs discover (kho_discover_noprsrv) and early does "
      "not%s; (b) the struct page "
      "init is in retrieve for baseline and in walk2 for early, the same "
      "work; (c) early walk1 also memblock_reserve()s every radix node "
      "page (kho_early_reserve_node), and those pages sit between the "
      "preserved ranges, so each one memmove()s the regions above it "
      "(O(nodes x reserved.cnt)). Measured (early, median over ok runs of "
      "(reserved.cnt after walk1 − reserved.cnt before walk1 − keys) / "
      "nodes, at the decomposition counts): %s; so walk1 adds fewer "
      "separate regions than keys + nodes (%s of the node count where "
      "positive; a node page that is adjacent to a reserved range merges "
      "into it instead of adding a region, an untested explanation); (d) "
      "after walk2, kho_radix_destroy_tree "
      "memblock_free()s the node pages again from an array of ~N regions "
      "(free_nodes, also O(nodes x reserved.cnt)); baseline frees no node "
      "pages during boot. (d), timed as free_nodes, and (c), inside walk1 "
      "and not separately timed, are the reasons from the code that early "
      "is slower than baseline (p < %.2f, ranges separated) at the listed "
      "counts: %s; the part of the TTI difference no timer covers is "
      "listed as the residual above." %
      ((" (contig %d: discover %s ms of the %+.0f ms early − baseline TTI "
        "difference)" % (dc["n"], fmt(dc["b"]["discover_ms"], 0),
                         dc["dtti"] * 1e3)) if dc else "",
       "; ".join(("%s %d: %.2f (median nodes %s)" %
                  (pt, e["count"], e["extra_per_node"], fint(e["nodes"])))
                 if e["extra_per_node"] > 0 else
                 ("%s %d: negative, keys merge (reserved.cnt %s < keys %s)"
                  % (pt, e["count"], fint(e["reserved_cnt"]),
                     fint(e["keys"])))
                 for pt, e in epn) or "no data",
       ("%.0f–%.0f%%" % (100 * min(epn_pos), 100 * max(epn_pos)))
       if epn_pos else "-",
       ALPHA,
       ", ".join("%s (%s)" % (pt, ", ".join(str(n) for n in ns))
                 for pt, ns in slower.items()) or "no pattern"))
    P("")
    P("## Correctness (full-content verify, fill=1, early variant)")
    P("")
    if vrows:
        P("| variant | pattern | count | status | verify | TTI s | log |")
        P("|---|---|---:|---|---|---:|---|")
        for r in vrows:
            P("| %s | %s | %s | %s | %s | %s | verify/%s |" %
              (r["variant"], r["pattern"], r["count"], r["status"],
               r["verify"], r["time_to_init_s"] or "-", r["logfile"]))
    else:
        P("No verify rows.")
    P("")
    P("## Traceability")
    P("")
    P("Every aggregated point lists its serial logs in `summary.csv` "
      "(column `logs`, relative to this directory). Each log has a "
      "`.harness.log` with the ssh-side evidence (A meminfo before/after "
      "the 1G hugepage free, B cmdline, B dmesg excerpt). The ENOMEM "
      "limits are parsed from the M1 probe serial logs named in the "
      "largest-swept-count table.")
    with open(out_md, "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote %s and %s (%d points, %d integrity problems)" %
          (out_md, out_csv, len(agg), len(probs)))
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main())
