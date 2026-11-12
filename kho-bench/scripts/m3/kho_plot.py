#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Plot M3 results (time to init vs preservation count, per pattern/mode).

Usage:
  kho_plot.py [--dir results/m3] [--sha SHA] [--outdir results/m3/graphs]

Reads <dir>/data.csv (status=ok rows of one bzImage sha, b_cmdline_mode
nohp) and writes PNG + SVG graphs plus plot_points.csv, which lists every
plotted value with the raw logs it comes from.  Medians are drawn as lines,
every single run as a dot, [min,max] as error bars.  b_ssh_timeout rows are
drawn as up-triangles at their measured lower bound (last printk timestamp
of kernel B) and are never connected to lines.  No fits, no extrapolation.
"""
import argparse
import collections
import csv
import os
import statistics
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kho_sweep  # noqa: E402

import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

PATTERNS = ["contig", "alt", "random", "mixed", "inverted"]
VARIANTS = kho_sweep.VARIANTS
# fixed colour per mode so that every graph uses the same legend
MODE_COLOR = dict(zip(VARIANTS, ["black", "tab:red", "tab:orange",
                                 "tab:olive", "tab:brown", "tab:blue",
                                 "tab:purple", "tab:pink", "tab:green"]))
COMPONENTS = [("walk1_ns", "walk1"), ("discover_ns", "discover"),
              ("retrieve_ns", "retrieve"), ("walk2_ns", "walk2"),
              ("free_nodes_ns", "free_nodes"),
              ("nodes_late_free_ns", "nodes_late_free"),
              ("memblock_free_all_ns", "memblock_free_all")]


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def load(d, sha):
    path = os.path.join(d, "data.csv")
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not sha:
        c = collections.Counter(r.get("bzimage_sha", "") for r in rows)
        sha = c.most_common(1)[0][0] if c else ""
    rows = [r for r in rows if r.get("bzimage_sha") == sha and
            r.get("b_cmdline_mode") == "nohp"]
    return rows, sha


def points(rows):
    pts = collections.defaultdict(lambda: {"ok": [], "to": []})
    for r in rows:
        key = (r["variant"], r["pattern"], int(r["count"]))
        if r["status"] == "ok" and fnum(r["time_to_init_s"]) is not None:
            pts[key]["ok"].append(r)
        elif "b_ssh_timeout" in r["status"]:
            pts[key]["to"].append(r)
    return pts


def save(fig, outdir, name):
    for ext in ("png", "svg"):
        fig.savefig(os.path.join(outdir, "%s.%s" % (name, ext)), dpi=120,
                    bbox_inches="tight")
    plt.close(fig)


def main():
    here = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(here, "results", "m3"))
    ap.add_argument("--sha")
    ap.add_argument("--outdir")
    a = ap.parse_args()
    outdir = a.outdir or os.path.join(a.dir, "graphs")
    os.makedirs(outdir, exist_ok=True)
    rows, sha = load(a.dir, a.sha)
    pts = points(rows)
    plotted = []

    def floor(v):
        p = pts.get((v, "none", 0))
        if p and p["ok"]:
            return statistics.median(float(r["time_to_init_s"])
                                     for r in p["ok"])
        return None

    def draw(ax, v, pat, overhead=False, record=True):
        keys = sorted(k for k in pts if k[0] == v and k[1] == pat)
        fl = floor(v) if overhead else 0.0
        if overhead and fl is None:
            return False
        xs, meds, lo, hi = [], [], [], []
        drawn = False
        for k in keys:
            p = pts[k]
            if p["ok"]:
                t = [float(r["time_to_init_s"]) - fl for r in p["ok"]]
                m = statistics.median(t)
                xs.append(k[2])
                meds.append(m)
                lo.append(m - min(t))
                hi.append(max(t) - m)
                ax.plot([k[2]] * len(t), t, ".", ms=3, alpha=0.4,
                        color="gray")
                if record:
                    plotted.append((v, pat, k[2], "overhead_median" if overhead
                                    else "median", m, len(t),
                                    ";".join(r["logfile"] for r in p["ok"])))
                drawn = True
            for r in p["to"]:
                lb = (fnum(r.get("b_last_ts_s")) or 0.0) - fl
                ax.plot([k[2]], [lb], "^", ms=8, color="red")
                ax.annotate("> %.0f s (timeout)" % lb, (k[2], lb),
                            fontsize=7)
                if record:
                    plotted.append((v, pat, k[2], "timeout_lower_bound", lb,
                                    1, r["logfile"]))
                drawn = True
        if xs:
            ax.errorbar(xs, meds, yerr=[lo, hi], marker="o", ms=4,
                        capsize=2, label=v, color=MODE_COLOR[v])
        return drawn

    # 1. TTI vs count per pattern, one line per mode
    for pat in PATTERNS:
        fig, ax = plt.subplots(figsize=(8, 5))
        any_ = False
        for v in VARIANTS:
            any_ |= draw(ax, v, pat)
        if not any_:
            plt.close(fig)
            continue
        fl = floor("early_opt")
        if fl is not None:
            ax.axhline(fl, ls="--", color="k", lw=0.8,
                       label="early_opt none/0 floor")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("preserved blocks (count, pattern %s)" % pat)
        ax.set_ylabel("time to init in kernel B (s)")
        ax.set_title("KHO M3: time to init vs count, %s (sha %s)" %
                     (pat, sha[:12]))
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=7)
        save(fig, outdir, "tti_vs_count_%s" % pat)

    # 1b. overview: one panel per pattern (log-log), all modes; and the
    # three headline modes with all patterns in one panel
    fig, axs = plt.subplots(2, 3, figsize=(16, 9))
    for ax, pat in zip(axs.flat, PATTERNS):
        for v in VARIANTS:
            draw(ax, v, pat, record=False)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_title(pat, fontsize=9)
        ax.set_xlabel("preserved blocks", fontsize=8)
        ax.set_ylabel("time to init (s)", fontsize=8)
        ax.grid(True, which="both", alpha=0.3)
    hs, ls_ = [], []
    for ax in axs.flat[:len(PATTERNS)]:
        for h, l in zip(*ax.get_legend_handles_labels()):
            if l not in ls_:
                hs.append(h)
                ls_.append(l)
    axs.flat[-1].axis("off")
    axs.flat[-1].legend(hs, ls_, loc="center", fontsize=9)
    fig.suptitle("KHO M3: time to init vs preserved blocks, all patterns "
                 "and modes (log-log, sha %s)" % sha[:12])
    save(fig, outdir, "tti_vs_count_overview")

    fig, ax = plt.subplots(figsize=(9, 6))
    styles = {"baseline": ":", "early": "--", "early_opt": "-"}
    colors = dict(zip(PATTERNS, plt.rcParams["axes.prop_cycle"].by_key()
                      ["color"]))
    for pat in PATTERNS:
        for v, lsty in styles.items():
            keys = sorted(k for k in pts if k[0] == v and k[1] == pat and
                          pts[k]["ok"])
            if not keys:
                continue
            xs = [k[2] for k in keys]
            ys = [statistics.median(float(r["time_to_init_s"])
                                    for r in pts[k]["ok"]) for k in keys]
            ax.plot(xs, ys, lsty, marker="o", ms=3, color=colors[pat],
                    label="%s %s" % (v, pat))
            for k in keys:
                for r in pts[k]["to"]:
                    ax.plot([k[2]], [fnum(r.get("b_last_ts_s")) or 0.0],
                            "^", color=colors[pat], ms=8)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("preserved blocks (count)")
    ax.set_ylabel("time to init in kernel B (s), median")
    ax.set_title("KHO M3: early_opt (solid) vs early (dashed) vs baseline "
                 "(dotted), same session")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=6, ncol=3)
    save(fig, outdir, "tti_vs_count_headline")

    # 2. overhead vs floor, per pattern
    for pat in PATTERNS:
        fig, ax = plt.subplots(figsize=(8, 5))
        any_ = False
        for v in VARIANTS:
            any_ |= draw(ax, v, pat, overhead=True)
        if not any_:
            plt.close(fig)
            continue
        ax.set_xscale("log")
        ax.set_xlabel("preserved blocks (count, pattern %s)" % pat)
        ax.set_ylabel("TTI - same-mode none/0 floor (s)")
        ax.set_title("KHO M3: overhead vs floor, %s" % pat)
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=7)
        save(fig, outdir, "overhead_vs_floor_%s" % pat)

    # 3. components at the largest count measured per pattern for >1 mode
    for pat in PATTERNS:
        counts = collections.Counter(k[2] for k in pts
                                     if k[1] == pat and pts[k]["ok"])
        cand = [c for c, n in counts.items() if n > 1]
        if not cand:
            continue
        cnt = max(cand)
        modes = [v for v in VARIANTS if pts.get((v, pat, cnt), {}).get("ok")]
        fig, ax = plt.subplots(figsize=(9, 5))
        bottom = [0.0] * len(modes)
        for col, lab in COMPONENTS + [(None, "rest of TTI")]:
            vals = []
            for i, v in enumerate(modes):
                rs = pts[(v, pat, cnt)]["ok"]
                if col:
                    x = [fnum(r.get(col)) for r in rs]
                    x = [y for y in x if y is not None]
                    vals.append(statistics.median(x) / 1e9 if x else 0.0)
                else:
                    tti = statistics.median(float(r["time_to_init_s"])
                                            for r in rs)
                    vals.append(max(0.0, tti - bottom[i]))
            ax.bar(modes, vals, bottom=bottom, label=lab)
            for i, v in enumerate(modes):
                plotted.append((v, pat, cnt, "component_" + lab, vals[i],
                                len(pts[(v, pat, cnt)]["ok"]),
                                ";".join(r["logfile"]
                                         for r in pts[(v, pat, cnt)]["ok"])))
            bottom = [b + x for b, x in zip(bottom, vals)]
        ax.set_ylabel("seconds (medians of ok runs)")
        ax.set_title("KHO M3: where the time goes, %s %d" % (pat, cnt))
        ax.tick_params(axis="x", rotation=30, labelsize=7)
        ax.legend(fontsize=7)
        save(fig, outdir, "components_%s_%d" % (pat, cnt))

    with open(os.path.join(outdir, "plot_points.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["mode", "pattern", "count", "stat", "value_s", "n",
                    "logfiles"])
        for p in plotted:
            w.writerow(p)
    print("wrote graphs to %s (%d plotted values, sha %s)" %
          (outdir, len(plotted), sha[:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
