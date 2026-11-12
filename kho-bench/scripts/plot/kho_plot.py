#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""KHO bench graphs + report.

Reads one or more harness data.csv files directly (raw per-boot rows),
re-checks every row against its raw serial log, recomputes per-point
median/min/max itself and writes:

  <out>/tti_vs_count.png|csv              TTI vs count, all series, log-log
  <out>/tti_vs_count_by_pattern.png|csv   same, one panel per pattern
  <out>/restore_phases_vs_count.png|csv   kernel B restore timers vs count
  <out>/reserved_cnt_vs_count.png|csv     memblock.reserved.cnt vs count
  <out>/report_numbers.json               numbers used by the report
  --report FILE                           report rendered from --template

Each CSV holds exactly the points drawn in the PNG of the same basename,
with the repo-relative serial logs behind every point.  No fitting, no
extrapolation, no simulated values.  Timeouts are drawn only as lower
bounds; flagged runs with a measured TTI are drawn as flagged values.

Example (from the repo root):
  python3 kho-bench/scripts/plot/kho_plot.py \\
      --data kho-bench/results/m2/data.csv --out kho-bench/graphs \\
      --report kho-bench/REPORT.md
Final command: add --data kho-bench/results/m3/data.csv (see README).
"""
import argparse
import collections
import csv
import json
import math
import os
import sys
import textwrap

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import khoplot_data as kd  # noqa: E402

import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker as mticker  # noqa: E402

PATTERN_ORDER = ["contig", "alt", "random", "mixed", "inverted"]
PATTERN_COLORS = {"contig": "tab:blue", "alt": "tab:orange",
                  "random": "tab:green", "mixed": "tab:purple",
                  "inverted": "tab:red"}
EXTRA_COLORS = ["tab:brown", "tab:pink", "tab:olive", "tab:cyan", "black"]
# mode styles: baseline / early fixed; other modes (M3) get the rest
MODE_STYLES = {"baseline": ("-", "o"), "early": ("--", "s")}
PHASE_COLORS = {"walk1_ns": "tab:blue", "walk2_ns": "tab:green",
                "free_nodes_ns": "tab:orange", "discover_ns": "tab:purple",
                "retrieve_ns": "tab:red", "memblock_free_all_ns": "tab:gray"}
# colour per variant when a panel is coloured by mode; a dataset that is not
# the last --data (e.g. m2 next to m3) gets dashed lines + hollow markers
VARIANT_COLORS = {"baseline": "tab:blue", "early": "tab:red",
                  "early_opt": "black", "early_bulk": "tab:green",
                  "early_nodefree": "tab:orange",
                  "early_coalesce": "tab:purple",
                  "early_noinit1": "tab:brown", "early_presize": "tab:pink",
                  "early_batchinit": "tab:olive"}
VARIANT_MARKERS = {"baseline": "o", "early": "s", "early_opt": "*",
                   "early_bulk": "D", "early_nodefree": "v",
                   "early_coalesce": "P", "early_noinit1": "<",
                   "early_presize": ">", "early_batchinit": "h"}
BASE_VARIANTS = ("baseline", "early")
CSV_COLS = ["graph", "panel", "dataset", "pattern", "mode", "variant",
            "count", "metric", "unit", "point_kind", "n_rows", "n_runs",
            "median", "min", "max", "plotted_value", "flags",
            "data_csv", "raw_logs"]


def default_repo_root():
    # kho-bench/scripts/plot -> repo root
    return os.path.normpath(os.path.join(HERE, "..", "..", ".."))


class Ctx:
    """Mode naming / styling / filtering shared by all graphs."""

    def __init__(self, rows, modes, patterns, floor_pattern):
        by_var = collections.defaultdict(set)
        for r in rows:
            by_var[r["variant"]].add(r["_dataset"])
        self.multi = {v for v, ds in by_var.items() if len(ds) > 1}
        seen = []
        self.mode_ds = {}
        self.datasets = []
        for r in rows:
            m = self.mode(r["_dataset"], r["variant"])
            if m not in seen:
                seen.append(m)
            self.mode_ds[m] = r["_dataset"]
            if r["_dataset"] not in self.datasets:
                self.datasets.append(r["_dataset"])
        if modes:
            want = [m.replace(":", "/") for m in modes]
            seen = [m for m in want if m in seen]
        self.modes = seen
        self.styles = {}
        for m in self.modes:
            v = m.split("/")[-1]
            self.styles[m] = MODE_STYLES.get(v) or (
                ":", VARIANT_MARKERS.get(v, "D"))
        pats = sorted({r["pattern"] for r in rows} - {floor_pattern},
                      key=lambda p: (PATTERN_ORDER.index(p)
                                     if p in PATTERN_ORDER else 99, p))
        if patterns:
            pats = [p for p in patterns if p in pats]
        self.patterns = pats
        self.floor_pattern = floor_pattern
        ex = iter(EXTRA_COLORS)
        self.colors = {p: PATTERN_COLORS.get(p) or next(ex, "black")
                       for p in pats}

    def mode(self, ds, variant):
        return "%s/%s" % (ds, variant) if variant in self.multi else variant

    def pmode(self, pt):
        return self.mode(pt.dataset, pt.variant)

    @staticmethod
    def variant(mode):
        return mode.split("/")[-1]

    def mode_style(self, mode):
        """(colour, linestyle, marker, filled) when colouring by mode.
        Same variant name in an older dataset: dashed + hollow."""
        v = self.variant(mode)
        old = self.mode_ds.get(mode) != (self.datasets[-1]
                                          if self.datasets else None)
        return (VARIANT_COLORS.get(v, "tab:cyan"), "--" if old else "-",
                VARIANT_MARKERS.get(v, "o"), not old)

    def ref_mode(self, mode, variant):
        """Mode of variant from the same dataset as mode (or None)."""
        ds = self.mode_ds.get(mode)
        m = self.mode(ds, variant)
        return m if m in self.modes and m != mode and \
            self.mode_ds.get(m) == ds else None


def g(x, nd=3):
    return "" if x is None else ("%.*f" % (nd, x))


def point_rows(graph, panel, pt, mode, metric, unit, nd=6):
    """CSV rows for the drawable parts of one aggregated point."""
    out = []
    base = dict(graph=graph, panel=panel, dataset=pt.dataset,
                pattern=pt.pattern, mode=mode, variant=pt.variant,
                count=pt.count, metric=metric, unit=unit,
                n_rows=pt.n_rows)
    if pt.ok_vals:
        md, mn, mx = kd.stats(pt.ok_vals)
        out.append(dict(base, point_kind="median" if len(pt.ok_vals) > 1
                        else "single_run", n_runs=len(pt.ok_vals),
                        median=g(md, nd), min=g(mn, nd), max=g(mx, nd),
                        plotted_value=g(md, nd),
                        flags="" if len(pt.ok_vals) > 1 else
                        "n=1 (one measurement, not a statistic)",
                        raw_logs=" ".join(pt.ok_logs)))
    for v, st, log in pt.flagged:
        out.append(dict(base, point_kind="flagged_measured", n_runs=1,
                        median=g(v, nd), min=g(v, nd), max=g(v, nd),
                        plotted_value=g(v, nd),
                        flags="status=%s; measured value from serial log, "
                              "not in ok statistics" % st, raw_logs=log))
    for lb, to, st, log in pt.lower:
        out.append(dict(base, point_kind="lower_bound", n_runs=1,
                        median="", min="", max="",
                        plotted_value=g(lb, nd),
                        flags="status=%s; no init line; TTI > %s s (last "
                              "kernel B printk); B_TIMEOUT %s s" %
                              (st, g(lb), g(to, 0)), raw_logs=log))
    for st, log in pt.failed:
        out.append(dict(base, point_kind="failed_no_tti", n_runs=1,
                        median="", min="", max="", plotted_value="",
                        flags="status=%s; no kernel B time to init; drawn "
                              "only as a labelled x-position marker, no "
                              "time value" % st, raw_logs=log))
    return out


def write_csv(path, rows, data_csv):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_COLS, lineterminator="\n")
        w.writeheader()
        for r in rows:
            r.setdefault("data_csv", data_csv.get(r["dataset"], ""))
            w.writerow({k: r.get(k, "") for k in CSV_COLS})


def floors(pts, ctx):
    """count-0 floor per mode: aggregated points of the floor pattern."""
    return {ctx.pmode(p): p for p in pts
            if p.pattern == ctx.floor_pattern and p.count == 0
            and ctx.pmode(p) in ctx.modes and p.ok_vals}


def max_measured(pt):
    """Largest measured TTI of a point: ok median, or a flagged value."""
    c = []
    if pt.ok_vals:
        c.append((kd.stats(pt.ok_vals)[0], "median"))
    c += [(v, "flagged") for v, _, _ in pt.flagged]
    return max(c) if c else None


def worst_case(pts, ctx):
    """Worst-case pattern from measured values only.
    rank: pattern -> (highest measured TTI, point, kind) over all modes.
    shared: per (mode, count) with >= 2 patterns measured (ok medians), the
    pattern with the highest median."""
    rank = {}
    for p in pts:
        if p.pattern not in ctx.patterns or ctx.pmode(p) not in ctx.modes:
            continue
        mm = max_measured(p)
        if mm and (p.pattern not in rank or mm[0] > rank[p.pattern][0]):
            rank[p.pattern] = (mm[0], p, mm[1])
    worst = max(rank, key=lambda k: rank[k][0]) if rank else None
    shared = collections.defaultdict(dict)
    for p in pts:
        if p.pattern in ctx.patterns and ctx.pmode(p) in ctx.modes \
                and p.ok_vals:
            shared[(ctx.pmode(p), p.count)][p.pattern] = \
                kd.stats(p.ok_vals)[0]
    top = {}
    for k, d in shared.items():
        if len(d) >= 2:
            top[k] = (max(d, key=d.get), len(d))
    return worst, rank, top


def series(pts, ctx, pattern, mode):
    return sorted((p for p in pts if p.pattern == pattern
                   and ctx.pmode(p) == mode), key=lambda p: p.count)


def draw_tti(ax, pts, ctx, patterns, worst, flo, graph, panel, rows,
             annotate_all=False, ann_at=(0.03, 0.70, 0.12), modes=None,
             color_by="pattern", annotate_ok=True):
    """Draw TTI series of the given patterns on ax, append CSV rows.
    modes: subset of ctx.modes (default all).  color_by: 'pattern' (line
    style by mode) or 'mode' (ctx.mode_style).  Annotations are stacked at
    axes-fraction position ann_at (x, y, dy).  Rows without a TTI
    (generate_failed etc.) get an x-position marker at the bottom of the
    axes and a label, never a time value."""
    ax_x, ax_y, ax_dy = ann_at
    modes = ctx.modes if modes is None else modes
    ann = []        # (y, text, x, color, weight), placed sorted by y below
    fails = []      # (x, text, color)
    for pat in patterns:
        hl = pat == worst and color_by == "pattern"
        for mode in modes:
            if color_by == "mode":
                col, ls, mk, filled = ctx.mode_style(mode)
                label = mode
            else:
                col = ctx.colors[pat]
                ls, mk = ctx.styles[mode]
                filled = True
                label = "%s / %s%s" % (pat, mode,
                                       "  (worst case)" if hl else "")
            mfc = col if filled else "white"
            ser = series(pts, ctx, pat, mode)
            if not ser:
                continue
            okp = [p for p in ser if p.ok_vals]
            if okp:
                xs = [p.count for p in okp]
                st = [kd.stats(p.ok_vals) for p in okp]
                ys = [s[0] for s in st]
                yerr = [[s[0] - s[1] for s in st], [s[2] - s[0] for s in st]]
                ax.errorbar(xs, ys, yerr=yerr, color=col, ls=ls, marker=mk,
                            mfc=mfc, ms=6 if hl else 5,
                            lw=2.6 if hl else 1.3, capsize=3, label=label)
                for p, s in zip(okp, st):
                    if len(p.ok_vals) < 3:
                        ax.plot([p.count], [s[0]], marker="o", ms=11,
                                mfc="none", mec=col, ls="none")
                        ax.annotate("n=%d" % len(p.ok_vals), (p.count, s[0]),
                                    xytext=(6, -12), textcoords="offset "
                                    "points", fontsize=7, color=col)
                if annotate_ok and (hl or annotate_all):
                    p, s = okp[-1], st[-1]
                    ann.append((s[0], "%s %s @%d:\nmedian %.*f s (n=%d)" %
                                (pat, mode, p.count, 1 if s[0] >= 10 else 3,
                                 s[0], len(p.ok_vals)), p.count, col, "bold"))
            for p in ser:
                for v, stt, _ in p.flagged:
                    ax.plot([p.count], [v], marker="X", ms=12, mfc="yellow",
                            mec=col, mew=1.8, ls="none")
                    ann.append((v, "FLAGGED (%s), %s %s @%d:\nmeasured "
                                "%.1f s (n=1, not a lower bound)" %
                                (stt, pat, mode, p.count, v), p.count, col,
                                "bold"))
                for lb, _, stt, _ in p.lower:
                    if lb is None:
                        continue
                    ax.errorbar([p.count], [lb], yerr=[[0], [lb * 0.6]],
                                lolims=True, color=col, marker="^", ms=10,
                                mfc="white", ls="none")
                    ann.append((lb, "TIMEOUT %s %s @%d:\nTTI > %.1f s "
                                "(lower bound only)" % (pat, mode, p.count,
                                                        lb), p.count, col,
                                "normal"))
                for stt, _ in p.failed:
                    fails.append((p.count, "%s %s @%d: %s\n(no kernel B "
                                  "TTI; not a time)" % (
                                      pat, mode, p.count,
                                      stt.split(";")[-1]), col))
                for r in point_rows(graph, panel, p, mode, "time_to_init",
                                    "s"):
                    rows.append(r)
    # stack the labels top-down in the order of their data values so that
    # the arrows do not cross
    for i, (y, txt, x, col, wt) in enumerate(sorted(ann, key=lambda a:
                                                    -a[0])):
        ax.annotate(txt, (x, y), xytext=(ax_x, ax_y - ax_dy * i),
                    textcoords="axes fraction", fontsize=8, color=col,
                    fontweight=wt, arrowprops=dict(arrowstyle="->",
                                                   color=col, lw=0.8))
    tr = ax.get_xaxis_transform()   # x in data, y in axes fraction
    for i, (x, txt, col) in enumerate(fails):
        ax.plot([x], [0.03], transform=tr, marker="x", ms=12, mew=2.5,
                color="crimson", ls="none", clip_on=False)
        ax.annotate(txt, (x, 0.03), xycoords=tr,
                    xytext=(-8, 14 + 26 * i), textcoords="offset points",
                    ha="right", fontsize=7, color="crimson",
                    arrowprops=dict(arrowstyle="->", color="crimson",
                                    lw=0.8))
    for mode, fp in flo.items():
        if mode not in modes:
            continue
        md, mn, mx = kd.stats(fp.ok_vals)
        lc, lls = ("0.45", ctx.styles[mode][0]) if color_by == "pattern" \
            else (ctx.mode_style(mode)[0], ":")
        ax.axhline(md, color=lc, ls=lls, lw=1,
                   label="count-0 floor %s: %.3f s (n=%d)" %
                   (mode, md, len(fp.ok_vals)))
        r = point_rows(graph, panel, fp, mode, "time_to_init", "s")[0]
        r["point_kind"] = "floor_reference_line"
        r["flags"] = "horizontal reference line (pattern %s, count 0)" % \
            fp.pattern
        rows.append(r)


def style_ax(ax, ylabel="time to init (s), log"):
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("preservation count (kho_bench.count), log")
    ax.set_ylabel(ylabel)
    ax.grid(True, which="major", alpha=0.4)
    ax.grid(True, which="minor", alpha=0.12)
    fmt = mticker.FuncFormatter(lambda v, _: "%g" % v)
    ax.yaxis.set_major_formatter(fmt)
    lo, hi = ax.get_ylim()
    if lo > 0 and hi / lo < 12:     # less than ~1 decade: label minor ticks
        ax.yaxis.set_minor_formatter(fmt)
    elif lo > 0 and hi / lo < 300:  # up to ~2.5 decades: label 2x and 5x
        ax.yaxis.set_minor_formatter(mticker.FuncFormatter(
            lambda v, _: "%g" % v if ("%.0e" % v)[0] in "25" else ""))
    else:
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())


def legend_notes():
    return ("median of status=ok runs; error bars = min/max;\nhollow ring "
            "+ n=k: fewer than 3 runs;\nyellow X = flagged run with a "
            "measured TTI\n(excluded from ok stats); ^ = timeout lower bound;"
            "\ncrimson x at the bottom = run without a kernel B TTI\n"
            "(e.g. generate_failed: ENOMEM in kernel A), no time drawn;\n"
            "every point is a measured VM boot\n(no fits, no "
            "extrapolation)")


def footer(txt, width=170):
    """Notes text re-wrapped into a few long lines for a figure footer."""
    return "\n".join(textwrap.wrap(" ".join(txt.split("\n")), width))


def dataset_note(ctx):
    if len(ctx.datasets) < 2:
        return ""
    return ("\n%s/<mode>: dataset %s (separate build/sweep); dashed line +"
            "\nhollow markers = older dataset when colouring by mode" %
            ("|".join(ctx.datasets), ", ".join(ctx.datasets)))


def best_mode(pts, ctx, worst):
    """Best improvement mode, computed: at the largest count of the worst
    pattern where >= 2 improvement modes have ok runs (1 if only one
    improvement mode has data there), the mode with the lowest median TTI.
    Returns (mode, count, {mode: median}) or Nones."""
    imp = [m for m in ctx.modes if ctx.variant(m) not in BASE_VARIANTS]
    med = collections.defaultdict(dict)
    for p in pts:
        if p.pattern == worst and ctx.pmode(p) in imp and p.ok_vals:
            med[p.count][ctx.pmode(p)] = kd.stats(p.ok_vals)[0]
    need = 2 if len({m for d in med.values() for m in d}) >= 2 else 1
    cs = sorted(c for c, d in med.items() if len(d) >= need)
    if not cs:
        return None, None, {}
    c = cs[-1]
    return min(med[c], key=med[c].get), c, med[c]


def headline_modes(pts, ctx, best):
    """baseline and early from the dataset with the most ok points for that
    variant, plus the best mode."""
    out = []
    for v in BASE_VARIANTS:
        cand = [m for m in ctx.modes if ctx.variant(m) == v]
        if cand:
            out.append(max(cand, key=lambda m: (sum(
                1 for p in pts if ctx.pmode(p) == m and p.ok_vals),
                -ctx.modes.index(m))))
    if best and best not in out:
        out.append(best)
    return out


def fig_tti_all(pts, ctx, worst, flo, out, data_csv, modes):
    rows = []
    fig, ax = plt.subplots(figsize=(13, 8.5))
    draw_tti(ax, pts, ctx, ctx.patterns, worst, flo, "tti_vs_count", "all",
             rows, ann_at=(0.47, 0.78, 0.075), modes=modes)
    style_ax(ax)
    ax.set_title("KHO restore: time to init vs preservation count\n"
                 "(modes: %s; worst-case pattern: %s; all modes per "
                 "pattern: tti_vs_count_by_pattern)" %
                 (", ".join(modes), worst), fontsize=10)
    ax.legend(fontsize=8, loc="upper left", ncol=2)
    fig.text(0.01, 0.005, footer(legend_notes() + dataset_note(ctx)),
             fontsize=8, va="bottom")
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    return save(fig, out, "tti_vs_count", rows, data_csv)


def panel_grid(n):
    ncol = 3
    nrow = max(1, math.ceil((n + 1) / ncol))
    fig, axes = plt.subplots(nrow, ncol, figsize=(6 * ncol, 4.6 * nrow),
                             squeeze=False)
    return fig, [a for row in axes for a in row]


def shared_legend(fig, axes, slot, note):
    """One legend for all panels (union of labels) in the spare axes."""
    hs = collections.OrderedDict()
    for ax in axes:
        for h, lab in zip(*ax.get_legend_handles_labels()):
            hs.setdefault(lab, h)
    slot.legend(list(hs.values()), list(hs.keys()), loc="upper left",
                fontsize=7.5, ncol=2, frameon=False)
    slot.text(0.0, 0.0, note, fontsize=8, va="bottom",
              transform=slot.transAxes)


def fig_tti_panels(pts, ctx, worst, flo, out, data_csv):
    rows = []
    fig, axes = panel_grid(len(ctx.patterns))
    fig.set_size_inches(19, 11)
    for ax, pat in zip(axes, ctx.patterns):
        draw_tti(ax, pts, ctx, [pat], worst, flo, "tti_vs_count_by_pattern",
                 pat, rows, color_by="mode", annotate_ok=False,
                 ann_at=(0.03, 0.93, 0.12))
        style_ax(ax)
        ax.set_title(pat + ("  — WORST CASE" if pat == worst else ""),
                     color="tab:red" if pat == worst else "black",
                     fontweight="bold" if pat == worst else "normal")
        if pat == worst:
            for s in ax.spines.values():
                s.set_edgecolor("tab:red")
                s.set_linewidth(2)
    for ax in axes[len(ctx.patterns):]:
        ax.axis("off")
    shared_legend(fig, axes[:len(ctx.patterns)], axes[len(ctx.patterns)],
                  legend_notes() + dataset_note(ctx))
    fig.suptitle("Time to init vs preservation count, per pattern, all "
                 "modes (colour = mode)")
    fig.tight_layout()
    return save(fig, out, "tti_vs_count_by_pattern", rows, data_csv)


def bar_count(pts, ctx, worst, ds):
    """Count of the worst pattern with ok runs in the most modes of dataset
    ds (ties: larger count)."""
    n = collections.Counter(p.count for p in pts if p.pattern == worst
                            and p.dataset == ds and p.ok_vals)
    return max(n, key=lambda c: (n[c], c)) if n else None


def fig_worst(pts, ctx, worst, flo, best, out, data_csv):
    rows = []
    fig, (ax, bx) = plt.subplots(1, 2, figsize=(19, 8),
                                 gridspec_kw=dict(width_ratios=(1.5, 1)))
    draw_tti(ax, pts, ctx, [worst], None, flo, "tti_worst_case",
             worst, rows, color_by="mode", annotate_ok=False,
             ann_at=(0.30, 0.93, 0.1))
    style_ax(ax)
    ax.legend(fontsize=7.5, loc="upper left", ncol=1)
    ax.set_title("worst-case pattern %s: time to init vs count, all modes"
                 % worst)
    ds = ctx.mode_ds.get(best) or (ctx.datasets[-1] if ctx.datasets
                                   else None)
    c = bar_count(pts, ctx, worst, ds)
    bp = [p for m in ctx.modes for p in pts if ctx.pmode(p) == m
          and p.pattern == worst and p.count == c and p.dataset == ds
          and p.ok_vals]
    for i, p in enumerate(bp):
        m = ctx.pmode(p)
        md, mn, mx = kd.stats(p.ok_vals)
        col = ctx.mode_style(m)[0]
        bx.bar(i, md, color=col, alpha=0.8, yerr=[[md - mn], [mx - md]],
               capsize=4)
        bx.text(i, mx * 1.08, "%.*f s\nn=%d" % (1 if md >= 10 else 3, md,
                                                len(p.ok_vals)),
                ha="center", fontsize=8)
        r = point_rows("tti_worst_case", "bars@%d" % c, p._replace(
            flagged=[], lower=[], failed=[]), m, "time_to_init", "s")[0]
        r["point_kind"] = "bar_" + r["point_kind"]
        rows.append(r)
    bx.set_yscale("log")
    bx.set_xticks(range(len(bp)))
    bx.set_xticklabels([ctx.pmode(p) for p in bp], rotation=35, ha="right",
                       fontsize=9)
    bx.set_ylabel("time to init (s), log")
    if bp:
        bx.set_ylim(min(kd.stats(p.ok_vals)[1] for p in bp) * 0.5,
                    max(kd.stats(p.ok_vals)[2] for p in bp) * 3)
    bx.yaxis.set_major_formatter(mticker.FuncFormatter(
        lambda v, _: "%g" % v))
    bx.grid(True, axis="y", alpha=0.4)
    bx.set_title("%s %s, dataset %s: every mode measured at this count "
                 "(median, min/max)" % (worst, c, ds), fontsize=10)
    fig.text(0.01, 0.005, footer(legend_notes() + dataset_note(ctx), 230),
             fontsize=7.5, va="bottom")
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    return save(fig, out, "tti_worst_case", rows, data_csv)


def fig_best(pts, ctx, flo, best, out, data_csv):
    rows = []
    fig, ax = plt.subplots(figsize=(13, 8))
    draw_tti(ax, pts, ctx, ctx.patterns, None,
             {m: f for m, f in flo.items() if m == best}, "tti_best_mode",
             "all", rows, annotate_all=True, ann_at=(0.03, 0.80, 0.09),
             modes=[best])
    style_ax(ax)
    ax.legend(fontsize=8, loc="upper right")
    ax.set_title("Best mode %s: time to init vs preservation count, all "
                 "patterns\n(best = lowest median at the largest worst-case"
                 "-pattern count measured by >= 2 improvement modes)" % best,
                 fontsize=10)
    fig.text(0.01, 0.005, footer(legend_notes()), fontsize=8,
             va="bottom")
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    return save(fig, out, "tti_best_mode", rows, data_csv)


def fig_metric_panels(rows_all, ctx, out, data_csv, name, cols, scale, unit,
                      title, ylabel, note, modes):
    """Generic companion graph: one panel per pattern, one line per
    (mode, column) for data.csv columns cols (value * scale).  Colour by
    column when there are several columns, else by mode."""
    rows = []

    def colour(col, mode):
        return PHASE_COLORS.get(col, "black") if len(cols) > 1 \
            else ctx.mode_style(mode)[0]
    fig, axes = panel_grid(len(ctx.patterns))
    for ax, pat in zip(axes, ctx.patterns):
        drawn = False
        for col in cols:
            pts = kd.aggregate([r for r in rows_all if r["pattern"] == pat],
                               kd.col_value(col, scale))
            for mode in modes:
                ls, mk = ctx.styles[mode]
                ser = [p for p in series(pts, ctx, pat, mode)]
                good = []
                for p in ser:
                    vals = [v for v in p.ok_vals]
                    if vals and kd.stats(vals)[0] > 0:
                        good.append(p)
                if good:
                    st = [kd.stats(p.ok_vals) for p in good]
                    ax.errorbar([p.count for p in good], [s[0] for s in st],
                                yerr=[[s[0] - max(s[1], s[0] * 1e-3)
                                       for s in st],
                                      [s[2] - s[0] for s in st]],
                                color=colour(col, mode), ls=ls,
                                marker=mk, ms=4, lw=1.3, capsize=2,
                                label="%s %s" % (mode,
                                                 col.replace("_ns", "")))
                    drawn = True
                    for p in good:
                        rows += point_rows(name, pat, p._replace(
                            flagged=[], lower=[], failed=[]), mode,
                            col.replace("_ns", ""), unit)
                for p in ser:
                    for v, stt, log in p.flagged:
                        if v > 0:
                            ax.plot([p.count], [v], marker="X", ms=10,
                                    mfc="yellow",
                                    mec=colour(col, mode),
                                    ls="none")
                            rows += point_rows(name, pat, p._replace(
                                ok_vals=[], lower=[], failed=[]), mode,
                                col.replace("_ns", ""), unit)
        style_ax(ax, ylabel)
        ax.set_title(pat)
        if drawn:
            ax.legend(fontsize=7, loc="upper left")
    for ax in axes[len(ctx.patterns):]:
        ax.axis("off")
    axes[len(ctx.patterns)].text(
        0.0, 0.5, "median of status=ok runs, error bars = min/max;\n"
        "yellow X = flagged run (measured, not in ok stats);\nzero medians "
        "cannot be drawn on a log axis and are omitted\n(they are not in "
        "the CSV either).\n" + note, fontsize=9, va="center")
    fig.suptitle(title)
    fig.tight_layout()
    return save(fig, out, name, rows, data_csv)


def save(fig, out, name, rows, data_csv):
    png = os.path.join(out, name + ".png")
    fig.savefig(png, dpi=110)
    plt.close(fig)
    write_csv(os.path.join(out, name + ".csv"), rows, data_csv)
    return name, len(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.
                                 RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", required=True,
                    help="data.csv path[:label] (repeatable); default label "
                    "= name of its directory, e.g. m2")
    ap.add_argument("--modes", nargs="*", help="modes (variants) to plot, "
                    "e.g. baseline early early_bulk; 'label/variant' when a "
                    "variant appears in several datasets")
    ap.add_argument("--patterns", nargs="*")
    ap.add_argument("--floor-pattern", default="none")
    ap.add_argument("--filter", action="append", default=[],
                    help="keep only rows with col=value (repeatable)")
    ap.add_argument("--out", default="kho-bench/graphs")
    ap.add_argument("--repo-root", default=default_repo_root())
    ap.add_argument("--report", help="write the report (markdown) here")
    ap.add_argument("--template",
                    default=os.path.join(HERE, "REPORT.template.md"))
    ap.add_argument("--no-verify-logs", action="store_true",
                    help="skip re-parsing the raw serial logs (not "
                    "recommended)")
    a = ap.parse_args(argv)

    root = os.path.abspath(a.repo_root)
    rows, info = kd.load(a.data, root, a.filter)
    probs, scans = ([], {}) if a.no_verify_logs else kd.verify_logs(rows)
    if probs:
        for p in probs:
            print("LOG MISMATCH:", p, file=sys.stderr)
        sys.exit("refusing to plot: %d rows disagree with their raw logs" %
                 len(probs))
    ctx = Ctx(rows, a.modes, a.patterns, a.floor_pattern)
    rows = [r for r in rows if ctx.mode(r["_dataset"], r["variant"])
            in ctx.modes]
    os.makedirs(a.out, exist_ok=True)
    data_csv = {i["label"]: i["path"] for i in info}

    pts = kd.aggregate(rows)
    flo = floors(pts, ctx)
    worst, rank, top = worst_case(pts, ctx)
    best, best_count, best_cmp = best_mode(pts, ctx, worst)
    hmodes = headline_modes(pts, ctx, best)
    hflo = {m: f for m, f in flo.items() if m in hmodes}
    made = [fig_tti_all(pts, ctx, worst, hflo, a.out, data_csv, hmodes),
            fig_tti_panels(pts, ctx, worst, hflo, a.out, data_csv)]
    if best:
        made += [fig_worst(pts, ctx, worst, hflo, best, a.out, data_csv),
                 fig_best(pts, ctx, flo, best, a.out, data_csv)]
    made += [
            fig_metric_panels(rows, ctx, a.out, data_csv,
                              "restore_phases_vs_count", kd.PHASE_COLS,
                              1e-6, "ms", "Kernel B restore timers vs "
                              "preservation count (early: walk1, walk2, "
                              "free_nodes; baseline: discover, retrieve; "
                              "both: memblock_free_all)",
                              "phase time (ms), log",
                              "Timers: kernel B KHO_BENCH lines, parsed into "
                              "data.csv\n(*_ns columns).  Baseline frees no "
                              "radix node pages\nduring boot, so it has no "
                              "free_nodes timer.", hmodes),
            fig_metric_panels(rows, ctx, a.out, data_csv,
                              "reserved_cnt_vs_count",
                              ["memblock_reserved_regions"], 1.0, "regions",
                              "memblock.reserved.cnt right after the KHO "
                              "reservations in kernel B vs preservation "
                              "count", "reserved.cnt (regions), log",
                              "Column memblock_reserved_regions of data.csv:"
                              "\nearly = after walk1, baseline = after "
                              "retrieve\n(KHO_BENCH 'reserved.cnt=a->b', "
                              "value b).", hmodes)]
    import kho_report  # noqa: E402  (local module)
    nums = kho_report.numbers(rows, pts, ctx, flo, worst, rank, top, info,
                              scans, probs, a)
    nums.update(kho_report.extra_numbers(rows, pts, ctx, worst, best,
                                         best_count, best_cmp, hmodes,
                                         [n for n, _ in made]))
    with open(os.path.join(a.out, "report_numbers.json"), "w") as f:
        json.dump(nums, f, indent=1, sort_keys=True)
        f.write("\n")
    for name, n in made:
        print("wrote %s/%s.png + .csv (%d plotted points)" % (a.out, name, n))
    print("rows used: %d from %s; raw-log re-check: %s" %
          (len(rows), ", ".join("%s (%s, sha256 %s)" %
                                (i["path"], i["label"], i["sha256"][:12])
                                for i in info),
           "skipped" if a.no_verify_logs else
           "%d logs, 0 mismatches" % len(scans)))
    print("worst-case pattern (highest measured TTI): %s" % worst)
    print("best mode (lowest median at %s %s): %s" % (worst, best_count,
                                                     best))
    if a.report:
        kho_report.render(a.template, a.report, nums, a, root)
        print("wrote %s" % a.report)


if __name__ == "__main__":
    main()
