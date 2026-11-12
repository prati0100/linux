# SPDX-License-Identifier: GPL-2.0
"""Report part of the KHO bench plotting pipeline.

numbers() collects every number the report uses from the aggregated data
(written to <out>/report_numbers.json); render() fills the {{KEY}}
placeholders of REPORT.template.md with markdown fragments built only from
those numbers.  The template holds prose without measured numbers.
"""
import collections
import os
import re

import khoplot_data as kd

BASE_MODES = ("baseline", "early")
# per-run cmdline tokens masked when listing kernel B's common cmdline
RUN_TOKENS = re.compile(r"\s*\b(kho_bench\.\w+|kho_reserve_mode|"
                        r"kho_reserve_opts)=\S*")


def f3(x):
    return "-" if x is None else "%.3f" % x


def f1(x):
    return "-" if x is None else "%.1f" % x


def pt_dict(p, ctx):
    md, mn, mx = kd.stats(p.ok_vals)
    return dict(dataset=p.dataset, variant=p.variant, mode=ctx.pmode(p),
                pattern=p.pattern, count=p.count, n_rows=p.n_rows,
                n_ok=len(p.ok_vals), median=md, min=mn, max=mx,
                logs=p.ok_logs,
                flagged=[dict(value=v, status=s, log=l)
                         for v, s, l in p.flagged],
                lower=[dict(lower_bound=lb, b_timeout_s=to, status=s,
                            log=l) for lb, to, s, l in p.lower],
                failed=[dict(status=s, log=l) for s, l in p.failed])


def common_count(okp, m, ref, pat):
    common = sorted(c for (mm, pp, c) in okp
                    if mm == m and pp == pat and (ref, pat, c) in okp)
    return common[-1] if common else None


def pair(e, b, m, ref, refv, pat, c, floor=None):
    bs, es = kd.stats(b.ok_vals), kd.stats(e.ok_vals)
    return dict(pattern=pat, mode=m, ref=ref, ref_variant=refv, count=c,
                ref_median=bs[0], ref_min=bs[1], ref_max=bs[2],
                ref_n=len(b.ok_vals), median=es[0], min=es[1], max=es[2],
                n=len(e.ok_vals), diff=es[0] - bs[0], ratio=es[0] / bs[0],
                speedup=bs[0] / es[0],
                separated=es[2] < bs[1] or bs[2] < es[1], floor=floor,
                ref_logs=b.ok_logs, logs=e.ok_logs)


def overlap(a, b):
    return not (a["min"] > b["max"] or b["min"] > a["max"])


def numbers(rows, pts, ctx, flo, worst, rank, top, info, scans, probs, a):
    n = {}
    n["inputs"] = info
    n["modes"] = ctx.modes
    n["patterns"] = ctx.patterns
    n["floor_pattern"] = ctx.floor_pattern
    n["rows_used"] = len(rows)
    n["status_counts"] = dict(collections.Counter(r["status"] for r in rows))
    n["class_counts"] = dict(collections.Counter(r["_class"] for r in rows))
    n["rows_with_tti"] = sum(1 for r in rows
                             if kd.fnum(r.get("time_to_init_s")) is not None)
    n["logs_rechecked"] = len(scans)
    n["log_mismatches"] = len(probs)
    n["points"] = [pt_dict(p, ctx) for p in pts]
    n["runs_per_point"] = dict(collections.Counter(
        str(p.n_rows) for p in pts))
    n["b_timeout_s"] = sorted({r.get("b_timeout_s", "") for r in rows})
    # environment, from data.csv columns and the raw logs (kernel B part)
    env = {}
    for col in ("kernel_build", "bzimage_sha", "kernel_git_desc",
                "b_cmdline_mode", "b_hugepages_1g_boot"):
        env[col] = dict(collections.Counter(r.get(col, "") for r in rows))
    env["b_cmdline_common"] = dict(collections.Counter(
        RUN_TOKENS.sub("", r.get("b_cmdline", "")).strip() for r in rows))
    env["b_mem_kb"] = dict(collections.Counter(
        str(s["mem_kb"]) for s in scans.values()))
    env["b_cpus_nodes"] = dict(collections.Counter(
        "%s CPUs, %s node(s)" % (s["cpus"], s["nodes"])
        for s in scans.values()))
    ts = sorted(r["timestamp"] for r in rows if r.get("timestamp"))
    env["first_timestamp"] = ts[0] if ts else ""
    env["last_timestamp"] = ts[-1] if ts else ""
    n["env"] = env
    n["floor"] = {m: pt_dict(p, ctx) for m, p in flo.items()}
    # worst case
    w = dict(pattern=worst, rank=[])
    for pat, (v, p, kind) in sorted(rank.items(), key=lambda x: -x[1][0]):
        w["rank"].append(dict(pattern=pat, highest_measured_s=v, kind=kind,
                              mode=ctx.pmode(p), count=p.count,
                              n_ok=len(p.ok_vals)))
    w["points"] = [pt_dict(p, ctx) for p in pts if p.pattern == worst]
    tops = collections.defaultdict(lambda: dict(top=[], not_top=[]))
    for (mode, cnt), (pat, k) in sorted(top.items(),
                                        key=lambda x: (x[0][0], x[0][1])):
        tops[mode]["top" if pat == worst else "not_top"].append(
            dict(count=cnt, top_pattern=pat, n_patterns=k))
    w["shared_count_ranking"] = dict(tops)
    n["worst"] = w
    # every mode vs baseline and vs early of the SAME dataset (same build
    # and sweep), at the largest count both have ok runs for, per pattern
    cmp_ = []
    okp = {(ctx.pmode(p), p.pattern, p.count): p for p in pts if p.ok_vals}
    for pat in ctx.patterns:
        for m in ctx.modes:
            for refv in BASE_MODES:
                if ctx.variant(m) == "baseline":
                    continue
                ref = ctx.ref_mode(m, refv)
                if ref is None:
                    continue
                c = common_count(okp, m, ref, pat)
                if c is None:
                    continue
                cmp_.append(pair(okp[(m, pat, c)], okp[(ref, pat, c)], m,
                                 ref, refv, pat, c,
                                 (n["floor"].get(m) or {}).get("median")))
    n["compare"] = cmp_
    n["improvement_modes"] = [m for m in ctx.modes
                              if m.split("/")[-1] not in BASE_MODES]
    n["datasets"] = ctx.datasets
    n["mode_dataset"] = {m: ctx.mode_ds.get(m) for m in ctx.modes}
    n["argv"] = dict(data=a.data, modes=a.modes, patterns=a.patterns,
                     filter=a.filter, out=a.out, report=a.report)
    return n


PHASES = ("walk1_ns", "free_nodes_ns", "walk2_ns", "memblock_free_all_ns")


def phase_medians(rows, ds, variant, pat, c):
    """Median (ms) of each PHASES column over the ok rows of one point."""
    out = {}
    for col in PHASES:
        v = [kd.fnum(r.get(col)) for r in rows
             if r["_dataset"] == ds and r["variant"] == variant and
             r["pattern"] == pat and int(r["count"]) == c and
             r["_class"] == "ok" and kd.fnum(r.get(col)) is not None]
        out[col] = kd.stats(v)[0] * 1e-6 if v else None
    return out


def extra_numbers(rows, pts, ctx, worst, best, best_count, best_cmp,
                  hmodes, graphs):
    """Numbers for the final (M2 + M3) report: best mode, its per-pattern
    ranking, cross-dataset comparisons, per-improvement-mode effects."""
    n = dict(best=None, headline_modes=hmodes, graphs=graphs)
    okp = {(ctx.pmode(p), p.pattern, p.count): p for p in pts if p.ok_vals}
    by_ds = {}
    for ds in ctx.datasets:
        rs = [r for r in rows if r["_dataset"] == ds]
        ts = sorted(r["timestamp"] for r in rs if r.get("timestamp"))
        by_ds[ds] = dict(
            rows=len(rs),
            kernel_build=dict(collections.Counter(
                r.get("kernel_build", "") for r in rs)),
            bzimage_sha=dict(collections.Counter(
                r.get("bzimage_sha", "") for r in rs)),
            modes=sorted({ctx.mode(ds, r["variant"]) for r in rs}),
            first_timestamp=ts[0] if ts else "",
            last_timestamp=ts[-1] if ts else "")
    n["by_dataset"] = by_ds
    n["failed_rows"] = [dict(mode=ctx.pmode(p), pattern=p.pattern,
                             count=p.count, status=s, log=l)
                        for p in pts for s, l in p.failed]
    if not best:
        return n
    bds = ctx.mode_ds.get(best)
    b = dict(mode=best, dataset=bds, rule_pattern=worst,
             rule_count=best_count,
             rule_medians=sorted(best_cmp.items(), key=lambda x: x[1]))
    # per pattern: the best mode's highest ok median
    rank = []
    for pat in ctx.patterns:
        ps = [p for (m, pp, c), p in okp.items() if m == best and pp == pat]
        if ps:
            top = max(ps, key=lambda p: kd.stats(p.ok_vals)[0])
            rank.append(pt_dict(top, ctx))
    rank.sort(key=lambda d: -d["median"])
    b["rank"] = rank
    b["top2_overlap"] = overlap(rank[0], rank[1]) if len(rank) > 1 else None
    # best vs baseline/early of the other datasets on the worst pattern
    # (different build and sweep: labelled cross-dataset)
    cross = []
    for m in ctx.modes:
        if ctx.variant(m) in BASE_MODES and ctx.mode_ds.get(m) != bds:
            c = common_count(okp, best, m, worst)
            if c is not None:
                cross.append(pair(okp[(best, worst, c)], okp[(m, worst, c)],
                                  best, m, ctx.variant(m), worst, c))
    b["cross"] = cross
    n["best"] = b
    # per improvement mode: effect vs same-dataset early per pattern
    eff = []
    for m in ctx.modes:
        if ctx.variant(m) in BASE_MODES:
            continue
        ref = ctx.ref_mode(m, "early")
        base = ctx.ref_mode(m, "baseline")
        ds = ctx.mode_ds.get(m)
        for pat in ctx.patterns:
            c = common_count(okp, m, ref, pat) if ref else None
            if c is None:
                continue
            d = pair(okp[(m, pat, c)], okp[(ref, pat, c)], m, ref, "early",
                     pat, c)
            bp = okp.get((base, pat, c)) if base else None
            d["base_median"] = kd.stats(bp.ok_vals)[0] if bp else None
            d["base_n"] = len(bp.ok_vals) if bp else 0
            d["phases"] = phase_medians(rows, ds, ctx.variant(m), pat, c)
            d["ref_phases"] = phase_medians(rows, ds, "early", pat, c)
            eff.append(d)
    n["effects"] = eff
    return n


# ---------------------------------------------------------------- render

def cnt_list(d):
    return "; ".join("`%s` (%d row%s)" % (k or "(empty)", v,
                                         "" if v == 1 else "s")
                     for k, v in sorted(d.items(), key=lambda x: -x[1]))


def key_modes(n):
    """headline modes (baseline, early, best) + same-variant modes of the
    other datasets, in ctx order."""
    hm = n.get("headline_modes") or n["modes"]
    hv = {m.split("/")[-1] for m in hm}
    return [m for m in n["modes"] if m in hm or (
        m.split("/")[-1] in BASE_MODES and m.split("/")[-1] in hv)]


def worst_lines(n, modes=None):
    """Per mode: the worst pattern's largest count with >= 3 ok runs, and
    every larger-count point (single runs, flagged values, lower bounds,
    rows without a TTI)."""
    w = n["worst"]
    out = []
    for m in (modes or n["modes"]):
        ps = sorted((p for p in w["points"] if p["mode"] == m),
                    key=lambda p: p["count"])
        stat = [p for p in ps if p["n_ok"] >= 3]
        if not stat:
            continue
        b = stat[-1]
        parts = ["median %.3f s at count %d (n=%d, range %.3f–%.3f s)" %
                 (b["median"], b["count"], b["n_ok"], b["min"], b["max"])]
        for p in ps:
            if p["count"] <= b["count"]:
                continue
            if p["n_ok"]:
                parts.append("%s %.3f s at count %d (%s)" % (
                    "median" if p["n_ok"] > 1 else "single ok run",
                    p["median"], p["count"], "n=%d" % p["n_ok"]))
            for x in p["flagged"]:
                parts.append("%.3f s at count %d (one run flagged `%s`; a "
                             "measured TTI, not a lower bound)" %
                             (x["value"], p["count"], x["status"]))
            for x in p["lower"]:
                parts.append("> %.3f s at count %d (timeout, lower bound "
                             "only)" % (x["lower_bound"], p["count"]))
            for x in p["failed"]:
                parts.append("no TTI at count %d (`%s`; kernel B not "
                             "measured)" % (p["count"], x["status"]))
        out.append("  - %s: %s." % (m, "; ".join(parts)))
    return out


def growth_lines(n):
    """Measured ratio of median TTI between consecutive counts (n_ok>=1)
    of the worst pattern, per mode.  Ratios of measured medians only."""
    w = n["worst"]
    out = []
    for m in n["modes"]:
        ps = sorted((p for p in w["points"] if p["mode"] == m and p["n_ok"]),
                    key=lambda p: p["count"])
        steps = ["%d→%d (count ×%.2f): TTI ×%.2f" % (
            a["count"], b["count"], b["count"] / a["count"],
            b["median"] / a["median"]) for a, b in zip(ps, ps[1:])]
        if steps:
            out.append("- %s: %s." % (m, "; ".join(steps)))
    return out


def frag_header(n, a):
    cmd = "python3 kho-bench/scripts/plot/kho_plot.py " + " ".join(
        ["--data %s" % d for d in n["argv"]["data"]] +
        (["--modes " + " ".join(n["argv"]["modes"])]
         if n["argv"]["modes"] else []) +
        (["--patterns " + " ".join(n["argv"]["patterns"])]
         if n["argv"]["patterns"] else []) +
        ["--filter %s" % f for f in n["argv"]["filter"]] +
        ["--out %s" % n["argv"]["out"], "--report %s" % n["argv"]["report"]])
    ins = "; ".join("`%s` (label %s, %d rows, %d used, sha256 `%s`)" %
                    (i["path"], i["label"], i["n_rows"], i["n_used"],
                     i["sha256"]) for i in n["inputs"])
    return ("> Generated by `kho-bench/scripts/plot/kho_plot.py` from %s. "
            "Every number below is computed by that script from the raw "
            "per-boot rows; do not edit numbers by hand — regenerate with:\n"
            ">\n> `%s`" % (ins, cmd))


def frag_conclusion(n):
    b = n.get("best")
    w = n["worst"]
    out = []
    if not b:
        return "- No improvement-mode data in the inputs."
    r0 = b["rank"][0]
    fl = (n["floor"].get(b["mode"]) or {})
    out.append("- **Best mode: `%s`** (dataset %s). Rule (computed): lowest "
               "median TTI at the largest `%s` count measured by at least "
               "two improvement modes (count %d: %s)." % (
                   b["mode"], b["dataset"], b["rule_pattern"],
                   b["rule_count"], ", ".join(
                       "%s %.3f s" % kv for kv in b["rule_medians"])))
    out.append("- **Highest median TTI of `%s` over all patterns and counts:"
               "** %s %d = **%.3f s** (n=%d, range %.3f–%.3f s), %+.3f s "
               "over its count-0 floor of %.3f s (n=%d)." % (
                   b["mode"], r0["pattern"], r0["count"], r0["median"],
                   r0["n_ok"], r0["min"], r0["max"],
                   r0["median"] - fl.get("median", 0), fl.get("median", 0),
                   fl.get("n_ok", 0)))
    if len(b["rank"]) > 1:
        r1 = b["rank"][1]
        out.append("  - Next highest: %s %d = %.3f s (n=%d, range %.3f–"
                   "%.3f s). %s" % (
                       r1["pattern"], r1["count"], r1["median"], r1["n_ok"],
                       r1["min"], r1["max"],
                       "The min–max ranges **overlap**, so for `%s` the "
                       "ranking of %s above %s is within noise (read it as "
                       "'highest median', not a separated worst case)." % (
                           b["mode"], r0["pattern"], r1["pattern"])
                       if b["top2_overlap"] else "The ranges are separated."))
    out.append("- **Worst-case pattern overall: `%s`** (highest measured TTI "
               "in any mode). Its measured TTI per mode:" % w["pattern"])
    out += worst_lines(n, key_modes(n))
    for c in n["compare"]:
        if c["mode"] == b["mode"] and c["pattern"] == w["pattern"]:
            out.append("- Same dataset, %s %d: `%s` %.3f s (n=%d) vs `%s` "
                       "%.3f s (n=%d): %s is ×%.1f faster (ranges %s)." % (
                           c["pattern"], c["count"], c["mode"], c["median"],
                           c["n"], c["ref"], c["ref_median"], c["ref_n"],
                           c["mode"], c["speedup"], "separated"
                           if c["separated"] else "overlap"))
    for c in b["cross"]:
        out.append("- Across datasets (different build and sweep; see "
                   "Environment), %s %d: `%s` %.3f s (n=%d) vs `%s` %.3f s "
                   "(n=%d), ratio ×%.1f." % (
                       c["pattern"], c["count"], c["mode"], c["median"],
                       c["n"], c["ref"], c["ref_median"], c["ref_n"],
                       c["speedup"]))
    for f in n.get("failed_rows", []):
        out.append("- %s %s %d has no TTI: `%s` (kernel B never booted; "
                   "not plotted as a time; log `%s`)." % (
                       f["mode"], f["pattern"], f["count"], f["status"],
                       f["log"]))
    return "\n".join(out)


def frag_summary(n):
    w = n["worst"]
    out = []
    if w["pattern"]:
        out.append("- **Worst case: `%s`.** It has the highest measured time "
                   "to init (TTI) of all tested patterns (ranking in "
                   "[Worst case](#6-worst-case)). Its measured TTI:" %
                   w["pattern"])
        out += worst_lines(n, key_modes(n))
        for mode, d in sorted(w["shared_count_ranking"].items()):
            if d["top"] and mode in key_modes(n):
                out.append("  - %s: `%s` has the highest median TTI at %d of "
                           "%d counts measured for 2+ patterns (counts %s%s)."
                           % (mode, w["pattern"], len(d["top"]),
                              len(d["top"]) + len(d["not_top"]),
                              ", ".join(str(x["count"]) for x in d["top"]),
                              "; highest ok median at the others: " +
                              ", ".join(
                                  "%d %s" % (x["count"], x["top_pattern"])
                                  for x in d["not_top"])
                              if d["not_top"] else ""))
    for m, fp in sorted(n["floor"].items()):
        if m not in key_modes(n):
            continue
        out.append("- Count-0 floor (no preservations), %s: median %.3f s "
                   "(n=%d, range %.3f–%.3f s)." % (m, fp["median"],
                                                   fp["n_ok"], fp["min"],
                                                   fp["max"]))
    best = (n.get("best") or {}).get("mode")
    sel = [c for c in n["compare"] if c["mode"] == best]
    if sel:
        out.append("- %s vs baseline/early of the same dataset (same build "
                   "and sweep), per pattern at the largest count measured "
                   "in both (median TTI; ranges = min–max of ok runs):" %
                   best)
        for c in sel:
            out.append("  - %s %d: %s %.3f s vs %s %.3f s (%+.3f s, ref/mode "
                       "×%.2f; n=%d/%d; %s)." % (
                           c["pattern"], c["count"], c["mode"], c["median"],
                           c["ref"], c["ref_median"], c["diff"], c["speedup"],
                           c["n"], c["ref_n"], "ranges separated"
                           if c["separated"] else "ranges overlap"))
    out.append("- Data: %d measured boots (rows) in %d points; statuses %s; "
               "kernel B printed its init line in %d of %d boots; every row "
               "re-checked against its raw serial log (%d logs, %d "
               "mismatches)." % (n["rows_used"], len(n["points"]),
                                 cnt_list(n["status_counts"]),
                                 n["rows_with_tti"], n["rows_used"],
                                 n["logs_rechecked"], n["log_mismatches"]))
    if not n["improvement_modes"]:
        out.append("- Improvements (M3): pending M3 — no improvement-mode "
                   "data in the inputs yet.")
    return "\n".join(out)


def frag_runs(n):
    rpp = ", ".join("%s rows: %d points" % (k, v) for k, v in
                    sorted(n["runs_per_point"].items(),
                           key=lambda x: -int(x[0])))
    return ("- Rows used: %d; row classes: %s.\n- Runs per point: %s.\n"
            "- Kernel B timeout (`b_timeout_s` column): %s s.\n"
            "- Measurement window (data.csv `timestamp`): %s – %s." % (
                n["rows_used"], cnt_list(n["class_counts"]), rpp,
                ", ".join(n["b_timeout_s"]), n["env"]["first_timestamp"],
                n["env"]["last_timestamp"]))


def frag_env(n):
    e = n["env"]
    out = ["- Dataset %s (`%s` rows): build %s; bzImage sha256 %s; boots %s "
           "– %s; modes %s." % (ds, d["rows"], cnt_list(d["kernel_build"]),
                                cnt_list(d["bzimage_sha"]),
                                d["first_timestamp"], d["last_timestamp"],
                                ", ".join("`%s`" % m for m in d["modes"]))
           for ds, d in sorted(n.get("by_dataset", {}).items())]
    out += ["- Kernel build (kernel B `Linux version` banner): %s." %
           cnt_list(e["kernel_build"]),
           "- bzImage sha256: %s." % cnt_list(e["bzimage_sha"]),
           "- git describe: %s." % cnt_list(e["kernel_git_desc"]),
           "- Kernel B memory known to the kernel (`Memory: …K/<total>K`, "
           "kB): %s." % "; ".join("%s (%d logs)" % kv for kv in
                                  sorted(e["b_mem_kb"].items())),
           "- Kernel B CPUs (`smp: Brought up`): %s." % "; ".join(
               "%s (%d logs)" % kv
               for kv in sorted(e["b_cpus_nodes"].items())),
           "- Kernel B 1G hugepages pre-allocated at boot: %s." %
           cnt_list(e["b_hugepages_1g_boot"]),
           "- Kernel B cmdline mode: %s; common kernel B cmdline (per-run "
           "`kho_bench.*`, `kho_reserve_mode=` removed): %s." % (
               cnt_list(e["b_cmdline_mode"]), cnt_list(e["b_cmdline_common"]))]
    return "\n".join(out)


def frag_graphs(n, rel):
    desc = {"tti_vs_count": "Headline: time to init vs preservation count, "
            "all patterns, modes %s (log-log)" %
            ", ".join("`%s`" % m for m in n.get("headline_modes", [])),
            "tti_vs_count_by_pattern": "One panel per pattern, every mode "
            "(colour = mode)",
            "tti_worst_case": "Worst-case pattern `%s`: every mode vs count "
            "(left) and every mode of the best mode's dataset at one shared "
            "count (right)" % n["worst"]["pattern"],
            "tti_best_mode": "Scaling of the best mode `%s` across all "
            "patterns" % (n.get("best") or {}).get("mode"),
            "restore_phases_vs_count": "Companion: kernel B restore timers "
            "(ms) vs count, headline modes",
            "reserved_cnt_vs_count": "Companion: memblock.reserved.cnt after "
            "the KHO reservations vs count, headline modes"}
    return "\n\n".join("**%s** — plotted points: [`%s.csv`](%s/%s.csv)\n\n"
                       "![%s](%s/%s.png)" % (desc.get(k, k), k, rel, k,
                                             desc.get(k, k), rel, k)
                       for k in n.get("graphs", desc))


def frag_tti_tables(n):
    out = []
    by = collections.defaultdict(dict)
    for p in n["points"]:
        by[(p["pattern"], p["count"])][p["mode"]] = p
    modes = n["modes"]
    for pat in n["patterns"]:
        out.append("\n#### %s\n" % pat)
        out.append("| count | " + " | ".join(
            "%s median s [min–max] (ok/rows)" % m for m in modes) + " |")
        out.append("|---:|" + "---:|" * len(modes))
        for (pp, c) in sorted(k for k in by if k[0] == pat):
            cells = []
            for m in modes:
                p = by[(pp, c)].get(m)
                if not p:
                    cells.append("")
                    continue
                s = ""
                if p["n_ok"]:
                    s = "%s [%s–%s] (%d/%d)" % (f3(p["median"]), f3(p["min"]),
                                                f3(p["max"]), p["n_ok"],
                                                p["n_rows"])
                for fl in p["flagged"]:
                    s += " **flagged %s: %s s**" % (fl["status"],
                                                   f3(fl["value"]))
                for lb in p["lower"]:
                    s += " **timeout: > %s s (lower bound)**" % \
                        f3(lb["lower_bound"])
                for fa in p["failed"]:
                    s += " failed: %s" % fa["status"]
                cells.append(s)
            out.append("| %d | %s |" % (c, " | ".join(cells)))
    return "\n".join(out)


def frag_worst(n):
    w = n["worst"]
    if not w["pattern"]:
        return "No data."
    out = ["Selection rule (computed): the pattern with the highest measured "
           "TTI (ok median or flagged measured value) over all modes and "
           "counts. Ranking:\n",
           "| pattern | highest measured TTI s | kind | mode | count | ok "
           "runs |", "|---|---:|---|---|---:|---:|"]
    for r in w["rank"]:
        out.append("| %s | %s | %s | %s | %d | %d |" % (
            r["pattern"], f3(r["highest_measured_s"]), r["kind"], r["mode"],
            r["count"], r["n_ok"]))
    out.append("\nGrowth of the worst-case pattern's median TTI between "
               "consecutive measured counts (ratios of measured medians; no "
               "fit):\n")
    out += growth_lines(n)
    out.append("\nMeasured TTI of the worst-case pattern `%s` at its three "
               "largest counts per mode:\n" % w["pattern"])
    out.append("| mode | count | ok/rows | median s | min s | max s | "
               "flagged (measured) | timeout lower bound | raw logs |")
    out.append("|---|---:|---:|---:|---:|---:|---|---|---|")
    for m in n["modes"]:
        ps = sorted((p for p in w["points"] if p["mode"] == m),
                    key=lambda p: p["count"])[-3:]
        for p in ps:
            out.append("| %s | %d | %d/%d | %s | %s | %s | %s | %s | %s |" % (
                m, p["count"], p["n_ok"], p["n_rows"], f3(p["median"]),
                f3(p["min"]), f3(p["max"]),
                "; ".join("%s s (%s)" % (f3(x["value"]), x["status"])
                          for x in p["flagged"]),
                "; ".join("> %s s" % f3(x["lower_bound"])
                          for x in p["lower"]),
                " ".join("`%s`" % l for l in
                         p["logs"] + [x["log"] for x in p["flagged"]] +
                         [x["log"] for x in p["lower"]])))
    return "\n".join(out)


def frag_workable(n):
    hm = set(n.get("headline_modes") or n["modes"])
    sel = [c for c in n["compare"] if c["mode"] in hm]
    if not sel:
        return "No mode/baseline pairs in the data."
    out = ["| pattern | largest count in both | mode | median s [min–max] "
           "(n) | reference (same dataset) median s [min–max] (n) | "
           "mode − ref s | ref / mode | mode − its count-0 floor s | "
           "ranges |",
           "|---|---:|---|---|---|---:|---:|---:|---|"]
    for c in sel:
        out.append("| %s | %d | %s | %s [%s–%s] (%d) | %s %s [%s–%s] (%d) | "
                   "%+.3f | ×%.2f | %s | %s |" % (
                       c["pattern"], c["count"], c["mode"], f3(c["median"]),
                       f3(c["min"]), f3(c["max"]), c["n"], c["ref"],
                       f3(c["ref_median"]), f3(c["ref_min"]),
                       f3(c["ref_max"]), c["ref_n"], c["diff"],
                       c["speedup"],
                       "-" if c["floor"] is None else
                       "%+.3f" % (c["median"] - c["floor"]),
                       "separated" if c["separated"] else "overlap"))
    return "\n".join(out)


def frag_improvements(n):
    if not n["improvement_modes"]:
        return ("**Pending M3.** The inputs contain only the modes %s; no "
                "improvement-mode data yet." %
                ", ".join("`%s`" % m for m in n["modes"]))
    out = ["Improvement modes in the inputs: %s. Each is compared with "
           "`early` (and `baseline`) **of the same dataset** at the largest "
           "count of each pattern measured in both. `early / mode` > 1 means "
           "the mode is faster. Phase timers are medians of the ok runs "
           "(ms; mode vs early)." % ", ".join(
               "`%s`" % m for m in n["improvement_modes"]), "",
           "| mode | pattern | count | mode median s [min–max] (n) | early "
           "median s (n) | early / mode | baseline median s (n) | baseline "
           "/ mode | ranges vs early | walk1 ms | free_nodes ms | walk2 ms "
           "| memblock_free_all ms |",
           "|---|---|---:|---|---|---:|---|---:|---|---|---|---|---|"]

    def ph(d, col):
        a, b = d["phases"][col], d["ref_phases"][col]
        return "%s vs %s" % (f1(a), f1(b))
    for d in n.get("effects", []):
        out.append("| %s | %s | %d | %s [%s–%s] (%d) | %s (%d) | ×%.2f | %s "
                   "| %s | %s | %s | %s | %s | %s |" % (
                       d["mode"], d["pattern"], d["count"], f3(d["median"]),
                       f3(d["min"]), f3(d["max"]), d["n"],
                       f3(d["ref_median"]), d["ref_n"], d["speedup"],
                       "%s (%d)" % (f3(d["base_median"]), d["base_n"])
                       if d["base_median"] is not None else "-",
                       "×%.2f" % (d["base_median"] / d["median"])
                       if d["base_median"] is not None else "-",
                       "separated" if d["separated"] else "overlap",
                       ph(d, "walk1_ns"), ph(d, "free_nodes_ns"),
                       ph(d, "walk2_ns"), ph(d, "memblock_free_all_ns")))
    return "\n".join(out)


def frag_flags(n):
    out = []
    for p in n["points"]:
        for x in p["flagged"]:
            out.append("- %s %s %d: flagged `%s`, measured TTI %s s from the "
                       "serial log (`%s`); drawn as a flagged value, not in "
                       "the ok statistics, not a lower bound." % (
                           p["mode"], p["pattern"], p["count"], x["status"],
                           f3(x["value"]), x["log"]))
        for x in p["lower"]:
            out.append("- %s %s %d: `%s`, no init line; only TTI > %s s "
                       "(lower bound) (`%s`)." % (
                           p["mode"], p["pattern"], p["count"], x["status"],
                           f3(x["lower_bound"]), x["log"]))
        for x in p["failed"]:
            out.append("- %s %s %d: `%s`, no TTI (`%s`)." % (
                p["mode"], p["pattern"], p["count"], x["status"], x["log"]))
        if 0 < p["n_ok"] < 3:
            out.append("- %s %s %d: only %d ok run(s) — %s." % (
                p["mode"], p["pattern"], p["count"], p["n_ok"],
                "one measurement, not a statistic" if p["n_ok"] == 1 else
                "min/max of 2 runs"))
    few = sorted({(p["n_ok"], p["n_rows"]) for p in n["points"]
                  if 3 <= p["n_ok"] < 5})
    if few:
        out.append("- Points with 3–4 ok runs: %d (see the tables)." %
                   sum(1 for p in n["points"] if 3 <= p["n_ok"] < 5))
    return "\n".join(out) if out else "- None."


def frag_trace(n, rel):
    return ("- Inputs: %s.\n- Rows whose TTI was re-derived from the raw "
            "serial log by the pipeline: %d (mismatches: %d; the script "
            "refuses to plot on any mismatch).\n- Every PNG in `%s/` has a "
            "CSV with the same basename listing exactly the plotted points; "
            "column `raw_logs` lists the serial logs (repo-relative) behind "
            "each point; `point_kind` is one of median, single_run, "
            "flagged_measured, lower_bound, floor_reference_line.\n"
            "- All report numbers: `%s/report_numbers.json`." % (
                "; ".join("`%s` (sha256 `%s`)" % (i["path"], i["sha256"])
                          for i in n["inputs"]),
                n["logs_rechecked"], n["log_mismatches"], rel, rel))


def render(template, path, n, a, root):
    rel = os.path.relpath(os.path.abspath(a.out),
                          os.path.dirname(os.path.abspath(path)))
    frags = dict(HEADER=frag_header(n, a), SUMMARY=frag_summary(n),
                 CONCLUSION=frag_conclusion(n),
                 RUNS=frag_runs(n), ENV=frag_env(n),
                 GRAPHS=frag_graphs(n, rel), TTI_TABLES=frag_tti_tables(n),
                 WORST=frag_worst(n), WORKABLE=frag_workable(n),
                 IMPROVEMENTS=frag_improvements(n), FLAGS=frag_flags(n),
                 TRACE=frag_trace(n, rel))
    with open(template) as f:
        txt = f.read()
    missing = set(re.findall(r"\{\{(\w+)\}\}", txt)) - set(frags)
    if missing:
        raise SystemExit("template placeholders without data: %s" %
                         sorted(missing))
    txt = re.sub(r"\{\{(\w+)\}\}", lambda m: frags[m.group(1)], txt)
    with open(path, "w") as f:
        f.write(txt)
