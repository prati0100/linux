#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Generate summary.md / summary.csv for the M3 sweep (all modes).

Usage:
  kho_summary.py [--dir results/m3] [--sha SHA] [--out summary.md]
                 [--agg-csv summary.csv]

M3 copy of the M2 summary generator, rewritten generically for any number of
modes (the M2 one hard-codes baseline/early and M2-specific text).

Only measured rows are used: status=ok rows (and, for verify rows,
verify=PASS) for statistics, rows of the selected bzImage sha (default: the
sha with most rows) with b_cmdline_mode=nohp.  A b_ssh_timeout row is
reported as a lower bound (TTI > last printk timestamp seen in B), never
extrapolated; nothing here fits curves.  Every row's time_to_init_s is
re-parsed from its raw serial log with kho_parse.parse() and mismatches are
listed.
"""
import argparse
import collections
import csv
import itertools
import os
import statistics
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kho_parse  # noqa: E402
import kho_sweep  # noqa: E402

PATTERNS = ["none", "contig", "alt", "random", "mixed", "inverted"]
VARIANTS = kho_sweep.VARIANTS
ALPHA = 0.05
TIMERS = ["walk1", "walk2", "free_nodes", "nodes_late_free", "discover",
          "retrieve", "memblock_free_all", "mfa_reserved_pages",
          "mfa_free_ranges", "count", "presize", "alloc", "collect", "sort",
          "bulk", "node"]


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


def fmt(x, nd=3):
    return "-" if x is None else ("%.*f" % (nd, x))


def mwu_p(a, b):
    """Exact two-sided Mann-Whitney U p-value (ties count 1/2)."""
    if len(a) < 2 or len(b) < 2 or len(a) + len(b) > 20:
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


RGN_B = 24  # sizeof(struct memblock_region) with CONFIG_NUMA on x86_64


def temp_mib(x):
    """Bulk temp array size (MiB) of a bulk/opt point, None otherwise."""
    if not x["variant"] in ("early_bulk", "early_opt") or \
            x["ranges"] is None:
        return None
    n = x["ranges"] + (x["nodes"] or 0)
    return ((n * RGN_B + 4095) // 4096) * 4096 / 2**20


def timer_col(t):
    return {"node": "node_ns"}.get(t, t + "_ns")


def aggregate(rows):
    pts = collections.OrderedDict()
    for r in rows:
        key = (r["variant"], r["pattern"], int(r["count"]))
        pts.setdefault(key, []).append(r)
    out = []
    for (v, pat, cnt), rs in pts.items():
        ok = [r for r in rs if r["status"] == "ok" and r["time_to_init_s"]]
        tti = [float(r["time_to_init_s"]) for r in ok]
        to = [r for r in rs if "b_ssh_timeout" in r["status"]]
        a = {"variant": v, "pattern": pat, "count": cnt, "n_rows": len(rs),
             "n_ok": len(ok), "n_timeout": len(to), "ttis": tti,
             "tti_med": med(tti), "tti_min": min(tti) if tti else None,
             "tti_max": max(tti) if tti else None,
             "tti_sd": statistics.stdev(tti) if len(tti) > 1 else None,
             "timeout_lb": max((fnum(r.get("b_last_ts_s")) or 0.0)
                               for r in to) if to else None,
             "regions": med([fnum(r["memblock_reserved_regions"])
                             for r in ok]),
             "ranges": med([fnum(r.get("ranges")) for r in ok]),
             "nodes": med([fnum(r.get("nodes")) for r in ok]),
             "reserved_max": med([fnum(r.get("reserved_max")) for r in ok]),
             "streams": sorted({int(r["streams"]) for r in ok
                                if r.get("streams") not in (None, "")}),
             "fallback": sorted({r.get("walk1_fallback", "") for r in ok
                                 if r.get("walk1_fallback") not in
                                 ("", "none")}),
             "logs": [r["logfile"] for r in rs]}
        for t in TIMERS:
            vals = [fnum(r.get(timer_col(t))) for r in ok]
            m = med(vals)
            a[t + "_ms"] = None if m is None else m / 1e6
        out.append(a)
    order = {v: i for i, v in enumerate(VARIANTS)}
    pord = {p: i for i, p in enumerate(PATTERNS)}
    out.sort(key=lambda a: (order.get(a["variant"], 99),
                            pord.get(a["pattern"], 99), a["count"]))
    return out


def tti_cell(a):
    if a["n_ok"]:
        s = "%s [%s,%s] (%d)" % (fmt(a["tti_med"]), fmt(a["tti_min"]),
                                 fmt(a["tti_max"]), a["n_ok"])
        if a["n_timeout"]:
            s += " +%d timeout" % a["n_timeout"]
        return s
    if a["n_timeout"]:
        return "> %s (timeout lower bound, %d run)" % (fmt(a["timeout_lb"]),
                                                      a["n_timeout"])
    return "no ok run (%d rows)" % a["n_rows"]


def main():
    here = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(here, "results", "m3"))
    ap.add_argument("--sha")
    ap.add_argument("--out")
    ap.add_argument("--agg-csv")
    ap.add_argument("--notes", help="markdown file inserted after the "
                    "header (hand-written interpretation)")
    a = ap.parse_args()
    d = a.dir
    out = a.out or os.path.join(d, "summary.md")
    agg_csv = a.agg_csv or os.path.join(d, "summary.csv")

    rows = read(os.path.join(d, "data.csv"))
    vrows = read(os.path.join(d, "verify", "data.csv"))
    allrows = rows + vrows
    sha = a.sha or (collections.Counter(r.get("bzimage_sha", "")
                                        for r in allrows).most_common(1) or
                    [("", 0)])[0][0]
    sel = [r for r in rows if r.get("bzimage_sha") == sha and
           r.get("b_cmdline_mode") == "nohp"]
    vsel = [r for r in vrows if r.get("bzimage_sha") == sha and
            r.get("b_cmdline_mode") == "nohp"]

    # raw-log re-check
    mism = []
    for base, rs in ((d, sel), (os.path.join(d, "verify"), vsel)):
        for r in rs:
            path = os.path.join(base, r["logfile"])
            if not os.path.exists(path):
                mism.append((r["logfile"], "missing log"))
                continue
            p = kho_parse.parse(path)
            if r["time_to_init_s"] and \
                    p.get("time_to_init_s", "") != r["time_to_init_s"]:
                mism.append((r["logfile"], "tti %s != %s" % (
                    r["time_to_init_s"], p.get("time_to_init_s"))))

    agg = aggregate(sel)
    idx = {(x["variant"], x["pattern"], x["count"]): x for x in agg}

    L = []
    P = L.append
    P("# KHO early-reserve benchmark: M3 summary (improvement modes)")
    P("")
    P("Generated %s by scripts/m3/kho_summary.py from %s (bzImage sha %s, "
      "%d TTI rows, %d verify rows). All values are measured VM boots; "
      "medians of status=ok runs, [min,max], (n). Timeouts are lower "
      "bounds, never extrapolated." %
      (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
       os.path.relpath(d, here), sha[:12], len(sel), len(vsel)))
    P("")
    P("Raw-log re-check: %d rows re-parsed, %d mismatches%s" %
      (len(sel) + len(vsel), len(mism), "" if not mism else ":"))
    for lf, why in mism[:50]:
        P("- %s: %s" % (lf, why))
    P("")

    # hand-written interpretation (optional), kept in a separate file so
    # that every table below stays script-generated
    if a.notes and os.path.exists(a.notes):
        with open(a.notes) as f:
            L.extend(f.read().rstrip("\n").split("\n"))
        P("")

    # row status counts
    P("## Row status counts")
    P("")
    P("| mode | rows | ok | b_ssh_timeout | generate_failed (ENOMEM in A) |"
      " other | b_warnings>0 |")
    P("|---|---|---|---|---|---|---|")
    bad = []
    for v in VARIANTS:
        rs = [r for r in sel + vsel if r["variant"] == v]
        if not rs:
            continue
        ok = sum(1 for r in rs if r["status"] == "ok")
        to = sum(1 for r in rs if "b_ssh_timeout" in r["status"])
        gf = sum(1 for r in rs if "generate_failed" in r["status"])
        bw = sum(1 for r in rs if (fnum(r.get("b_warnings")) or 0) > 0)
        P("| %s | %d | %d | %d | %d | %d | %d |" % (
            v, len(rs), ok, to, gf, len(rs) - ok - to - gf, bw))
        bad += [r for r in rs if r["status"] != "ok" or
                (fnum(r.get("b_warnings")) or 0) > 0]
    if bad:
        P("")
        P("Rows that are not status=ok or have kernel-B warnings:")
        P("")
        for r in bad:
            P("- %s %s %s: status=%s b_warnings=%s b_last_ts_s=%s log=%s" % (
                r["variant"], r["pattern"], r["count"], r["status"],
                r.get("b_warnings", ""), r.get("b_last_ts_s", ""),
                r["logfile"]))
    P("")

    # overview: one row per (pattern, count), one column per mode
    modes_present = [v for v in VARIANTS if any(x["variant"] == v
                                                for x in agg)]
    P("## Overview: median TTI (s) per pattern x count x mode")
    P("")
    P("Cell = median of ok runs (n); `>x` = timeout lower bound; empty = "
      "not measured.")
    P("")
    P("| pattern | count | " + " | ".join(modes_present) + " |")
    P("|---|---|" + "---|" * len(modes_present))
    pcs = sorted({(x["pattern"], x["count"]) for x in agg},
                 key=lambda pc: (PATTERNS.index(pc[0]) if pc[0] in PATTERNS
                                 else 99, pc[1]))
    for pat, cnt in pcs:
        cells = []
        for v in modes_present:
            x = idx.get((v, pat, cnt))
            if not x:
                cells.append("")
            elif x["n_ok"]:
                cells.append("%s (%d)" % (fmt(x["tti_med"]), x["n_ok"]))
            elif x["n_timeout"]:
                cells.append(">%s" % fmt(x["timeout_lb"], 0))
            else:
                cells.append("fail")
        P("| %s | %d | %s |" % (pat, cnt, " | ".join(cells)))
    P("")

    # per mode TTI tables
    P("## Time to init (s) per mode")
    for v in VARIANTS:
        xs = [x for x in agg if x["variant"] == v]
        if not xs:
            continue
        P("")
        P("### %s" % v)
        P("")
        P("| pattern | count | TTI median [min,max] (n) | sd | regions | "
          "walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | "
          "mfa ms | fallback |")
        P("|---|---|---|---|---|---|---|---|---|---|---|")
        for x in xs:
            rr = x["walk1_ms"] if x["walk1_ms"] is not None else \
                x["retrieve_ms"]
            P("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                x["pattern"], x["count"], tti_cell(x), fmt(x["tti_sd"]),
                fmt(x["regions"], 0), fmt(rr, 1), fmt(x["walk2_ms"], 1),
                fmt(x["free_nodes_ms"], 1), fmt(x["nodes_late_free_ms"], 3),
                fmt(x["memblock_free_all_ms"], 1),
                ",".join(x["fallback"]) or "-"))

    # walk1 split for modes with sub-timers
    P("")
    P("## walk1 sub-timers (ms, medians)")
    P("")
    P("streams = number of sorted streams merged by memblock_reserve_bulk() "
      "(set of values seen in the ok runs); bulk cost is O(streams x "
      "(n + R)) for n new ranges and R existing regions.  reserved array "
      "MiB = reserved.max x %d B (sizeof(struct memblock_region), x86_64 "
      "NUMA) from the logged max; temp MiB = page-aligned (ranges + nodes) "
      "x %d B, the bulk/opt temporary array, allocated while noprsrv-only "
      "(KHO scratch) is on and freed after walk 1.  Both are derived from "
      "logged counts, not measured allocations." % (RGN_B, RGN_B))
    P("")
    P("| mode | pattern | count | walk1 | count | presize | alloc | "
      "collect | sort | bulk | node | ranges | streams | reserved.max | "
      "reserved array MiB | temp MiB |")
    P("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for x in agg:
        if x["walk1_ms"] is None or x["variant"] == "early":
            continue
        P("| %s | %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |"
      " %s | %s | %s |" % (
            x["variant"], x["pattern"], x["count"], fmt(x["walk1_ms"], 2),
            fmt(x["count_ms"], 2), fmt(x["presize_ms"], 2),
            fmt(x["alloc_ms"], 3), fmt(x["collect_ms"], 2),
            fmt(x["sort_ms"], 2), fmt(x["bulk_ms"], 2),
            fmt(x["node_ms"], 2), fmt(x["ranges"], 0),
            ",".join(str(v) for v in x["streams"]) or "-",
            fmt(x["reserved_max"], 0),
            fmt(None if x["reserved_max"] is None else
                x["reserved_max"] * RGN_B / 2**20, 1),
            fmt(temp_mib(x), 1)))

    # mode vs early / baseline
    for ref in ("early", "baseline"):
        P("")
        P("## mode minus %s (median TTI, same pattern and count)" % ref)
        P("")
        P("| mode | pattern | count | mode TTI | %s TTI | delta s | p (MWU) |"
          % ref)
        P("|---|---|---|---|---|---|---|")
        for x in agg:
            if x["variant"] in (ref, "baseline") or not x["n_ok"]:
                continue
            y = idx.get((ref, x["pattern"], x["count"]))
            if not y:
                continue
            if y["n_ok"]:
                dlt = "%+.3f" % (x["tti_med"] - y["tti_med"])
            elif y["n_timeout"]:
                dlt = "< %+.3f" % (x["tti_med"] - y["timeout_lb"])
            else:
                continue
            p = mwu_p(x["ttis"], y["ttis"])
            P("| %s | %s | %d | %s | %s | %s | %s |" % (
                x["variant"], x["pattern"], x["count"], tti_cell(x),
                tti_cell(y), dlt, fmt(p, 3)))

    # worst case per mode
    P("")
    P("## Worst case per mode (highest median TTI, or timeout lower bound)")
    P("")
    P("| mode | pattern | count | TTI | floor (none/0) | overhead vs floor s |")
    P("|---|---|---|---|---|---|")
    for v in VARIANTS:
        xs = [x for x in agg if x["variant"] == v and x["pattern"] != "none"]
        if not xs:
            continue

        def worst_key(x):
            return max(x["tti_med"] or 0.0, x["timeout_lb"] or 0.0)
        w = max(xs, key=worst_key)
        fl = idx.get((v, "none", 0))
        flv = fl["tti_med"] if fl else None
        ov = None if flv is None else worst_key(w) - flv
        P("| %s | %s | %d | %s | %s | %s%s |" % (
            v, w["pattern"], w["count"], tti_cell(w), fmt(flv),
            "> " if not w["n_ok"] else "", fmt(ov)))

    # largest count per pattern measured for >1 mode: overhead vs each
    # mode's own floor and ratio vs early / baseline (same session)
    P("")
    P("## Largest same-session comparison point per pattern")
    P("")
    P("Overhead = mode median TTI - same mode none/0 median TTI.  Ratios "
      "use medians of this session only.")
    P("")
    P("| pattern | count | mode | TTI | overhead vs floor s | "
      "x early | x baseline |")
    P("|---|---|---|---|---|---|---|")
    for pat in PATTERNS[1:]:
        cnts = collections.Counter(x["count"] for x in agg
                                   if x["pattern"] == pat and
                                   (x["n_ok"] or x["n_timeout"]))
        cand = [c for c, n in cnts.items() if n > 1]
        # the largest count where early or baseline were also measured
        ref = [c for c in cand if idx.get(("early", pat, c)) or
               idx.get(("baseline", pat, c))]
        for cnt in sorted(set([max(cand)] if cand else []) |
                          set([max(ref)] if ref else [])):
            def val(v):
                x = idx.get((v, pat, cnt))
                if not x:
                    return None, None
                if x["n_ok"]:
                    return x["tti_med"], ""
                if x["n_timeout"]:
                    return x["timeout_lb"], ">"
                return None, None
            e, es = val("early")
            b, bs = val("baseline")
            for v in VARIANTS:
                x = idx.get((v, pat, cnt))
                if not x:
                    continue
                t, ts = val(v)
                fl = idx.get((v, "none", 0))
                flv = fl["tti_med"] if fl else None
                ov = None if t is None or flv is None else t - flv

                def ratio(r, rs):
                    if t is None or r is None or (ts and rs):
                        return "-"
                    # a timeout lower bound on one side bounds the ratio
                    # in one direction only
                    return "%s%.3f" % ("> " if ts else "< " if rs else "",
                                       t / r)
                P("| %s | %d | %s | %s | %s%s | %s | %s |" % (
                    pat, cnt, v, tti_cell(x), ts or "", fmt(ov),
                    ratio(e, es), ratio(b, bs)))

    # verify
    P("")
    P("## Correctness (verify rows, fill=1, kho_bench full-content verify)")
    P("")
    P("| mode | pattern | count | status | verify | check | log |")
    P("|---|---|---|---|---|---|---|")
    for r in vsel:
        P("| %s | %s | %s | %s | %s | %s | verify/%s |" % (
            r["variant"], r["pattern"], r["count"], r["status"], r["verify"],
            r.get("check", "") or "-", r["logfile"]))

    with open(out, "w") as f:
        f.write("\n".join(L) + "\n")

    cols = ["variant", "pattern", "count", "n_rows", "n_ok", "n_timeout",
            "tti_med", "tti_min", "tti_max", "tti_sd", "timeout_lb",
            "regions", "ranges"] + [t + "_ms" for t in TIMERS] + ["logs"]
    with open(agg_csv, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for x in agg:
            w.writerow([";".join(x[c]) if c == "logs" else
                        ("" if x[c] is None else x[c]) for c in cols])
    print("wrote %s and %s (%d points, %d mismatches)" %
          (out, agg_csv, len(agg), len(mism)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
