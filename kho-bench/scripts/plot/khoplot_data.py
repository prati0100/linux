# SPDX-License-Identifier: GPL-2.0
"""Data layer of the KHO bench plotting/report pipeline.

Reads the harness data.csv files directly (one row per measured VM boot),
re-checks every row's time to init against its raw serial log, and
aggregates rows into plot points.  Nothing here fits, extrapolates or
invents values: every number is either a value from a data.csv row (which
was parsed from a serial log by the harness) or a median/min/max over such
values.

Row classes (per data.csv row):
  ok          status == "ok" and time_to_init_s present -> used for the
              median/min/max statistics.
  flagged     time_to_init_s present but status != "ok" (e.g. a
              b_ssh_timeout run whose serial log still contains kernel B's
              "Run /sbin/init" line).  A measured value, plotted with its
              own marker, never part of the ok statistics.
  lower_bound no time_to_init_s and status contains b_ssh_timeout: kernel B
              never reached init while it was observed.  Only a lower bound
              (b_last_ts_s = last printk timestamp kernel B printed).
  failed      anything else without a time to init (e.g. generate failed
              in kernel A).  Listed, never plotted as a timing.
"""
import collections
import csv
import hashlib
import os
import re
import statistics

TS = r"\[\s*(\d+\.\d+)\]"
RE_BANNER = re.compile(TS + r" Linux version ")
RE_INIT = re.compile(TS + r" Run /sbin/init as init process")
RE_TS = re.compile(TS)
RE_MEM = re.compile(r"\] Memory: \d+K/(\d+)K available")
RE_SMP = re.compile(r"smp: Brought up (\d+) nodes?, (\d+) CPUs")

# timer columns of data.csv (ns) that the companion graph can plot
PHASE_COLS = ["walk1_ns", "walk2_ns", "free_nodes_ns", "discover_ns",
              "retrieve_ns", "memblock_free_all_ns"]


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def parse_spec(spec):
    """'path[:label]' -> (path, label).  Default label: the name of the
    directory that holds data.csv (e.g. 'm2')."""
    path, label = spec, None
    if ":" in spec and not os.path.exists(spec):
        path, label = spec.rsplit(":", 1)
    if not label:
        label = os.path.basename(os.path.dirname(os.path.abspath(path)))
    return path, label


def load(specs, repo_root, filters=()):
    """Load rows of every data.csv in specs.  Each row gets extra keys:
    _dataset (label), _csv (repo-relative csv path), _log (repo-relative
    serial log path), _log_abs, _class.  filters: list of 'col=value';
    rows not matching every filter are dropped (and counted)."""
    rows, info = [], []
    flt = [f.split("=", 1) for f in filters]
    for spec in specs:
        path, label = parse_spec(spec)
        base = os.path.dirname(os.path.abspath(path))
        n_all = n_used = 0
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                n_all += 1
                if any(r.get(k, "") != v for k, v in flt):
                    continue
                n_used += 1
                r["_dataset"] = label
                r["_csv"] = os.path.relpath(os.path.abspath(path), repo_root)
                r["_log_abs"] = os.path.normpath(
                    os.path.join(base, r["logfile"]))
                r["_log"] = os.path.relpath(r["_log_abs"], repo_root)
                r["_class"] = classify(r)
                rows.append(r)
        info.append(dict(label=label, path=os.path.relpath(
            os.path.abspath(path), repo_root), sha256=sha256(path),
            n_rows=n_all, n_used=n_used))
    return rows, info


def classify(r):
    tti = fnum(r.get("time_to_init_s"))
    st = r.get("status", "")
    if tti is not None:
        return "ok" if st == "ok" else "flagged"
    if "b_ssh_timeout" in st:
        return "lower_bound"
    return "failed"


def log_scan(path):
    """Independent minimal parse of one serial log: kernel B is the text
    after the 2nd 'Linux version' banner (timestamps restart at kexec).
    Returns dict(tti=str|'', last_ts=str|'', mem_kb, cpus, nodes)."""
    out = dict(tti="", last_ts="", mem_kb=None, cpus=None, nodes=None)
    nb = 0
    with open(path, errors="replace") as f:
        for line in f:
            if RE_BANNER.search(line):
                nb += 1
                if nb > 2:      # only kernel B (2nd kernel) is of interest
                    break
                continue
            if nb != 2:
                continue
            m = RE_INIT.search(line)
            if m:
                out["tti"] = m.group(1)
            m = None
            for m in RE_TS.finditer(line):
                pass
            if m:
                out["last_ts"] = m.group(1)
            m = RE_MEM.search(line)
            if m:
                out["mem_kb"] = int(m.group(1))
            m = RE_SMP.search(line)
            if m:
                out["nodes"], out["cpus"] = int(m.group(1)), int(m.group(2))
    return out


def verify_logs(rows):
    """Re-derive each row's TTI (and lower bound) from its raw serial log.
    Returns (problems, scans) where scans maps _log -> log_scan()."""
    probs, scans = [], {}
    for r in rows:
        if not os.path.exists(r["_log_abs"]):
            probs.append("missing log %s" % r["_log"])
            continue
        s = scans[r["_log"]] = log_scan(r["_log_abs"])
        if s["tti"] != r.get("time_to_init_s", ""):
            probs.append("%s: data.csv time_to_init_s %r != log %r" %
                         (r["_log"], r.get("time_to_init_s", ""), s["tti"]))
        if r["_class"] == "lower_bound" and \
                s["last_ts"] != r.get("b_last_ts_s", ""):
            probs.append("%s: data.csv b_last_ts_s %r != log %r" %
                         (r["_log"], r.get("b_last_ts_s", ""), s["last_ts"]))
    return probs, scans


Point = collections.namedtuple(
    "Point", "dataset variant pattern count n_rows ok_vals ok_logs flagged "
             "lower failed")


def tti_value(r):
    return fnum(r.get("time_to_init_s"))


def col_value(col, scale=1.0):
    def f(r):
        v = fnum(r.get(col))
        return None if v is None else v * scale
    return f


def aggregate(rows, value=tti_value):
    """Group rows by (dataset, variant, pattern, count).  value(row) gives
    the metric of a row.  ok rows with a value go to ok_vals; flagged rows
    with a value go to flagged [(value, status, log)]; lower_bound rows to
    lower [(b_last_ts_s, b_timeout_s, status, log)]; the rest to failed."""
    grp = collections.OrderedDict()
    for r in rows:
        k = (r["_dataset"], r["variant"], r["pattern"], int(r["count"]))
        grp.setdefault(k, []).append(r)
    pts = []
    for (ds, v, p, n), rs in grp.items():
        ok_vals, ok_logs, fl, lo, fa = [], [], [], [], []
        for r in rs:
            c = r["_class"]
            x = value(r)
            if c == "ok" and x is not None:
                ok_vals.append(x)
                ok_logs.append(r["_log"])
            elif c == "flagged" and x is not None:
                fl.append((x, r["status"], r["_log"]))
            elif c == "lower_bound":
                lo.append((fnum(r.get("b_last_ts_s")),
                           fnum(r.get("b_timeout_s")), r["status"], r["_log"]))
            elif c not in ("ok", "flagged"):
                fa.append((r["status"], r["_log"]))
        pts.append(Point(ds, v, p, n, len(rs), ok_vals, ok_logs, fl, lo, fa))
    return pts


def stats(vals):
    if not vals:
        return None, None, None
    return statistics.median(vals), min(vals), max(vals)
