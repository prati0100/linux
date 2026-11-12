# SPDX-License-Identifier: GPL-2.0
"""Unit tests for the KHO bench plotting pipeline (synthetic inputs only).

Run: python3 kho-bench/scripts/plot/test_kho_plot.py
"""
import contextlib
import csv
import io
import json
import os
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import khoplot_data as kd  # noqa: E402
import kho_plot  # noqa: E402

COLS = ["variant", "pattern", "count", "run", "time_to_init_s",
        "memblock_reserved_regions", "logfile", "status", "walk1_ns",
        "retrieve_ns", "b_last_ts_s", "b_timeout_s", "keys"]


def log_text(tti_b, last_ts=None, a_init="1.000000"):
    lines = ["[    0.000000] Linux version 7.4 (x) #1 SMP",
             "[    %s] Run /sbin/init as init process" % a_init,
             "[    0.000000] Linux version 7.4 (x) #1 SMP",
             "[    0.100000] Memory: 100K/2000K available",
             "[    0.200000] smp: Brought up 1 node, 8 CPUs"]
    if tti_b:
        lines.append("Failed [  %s] Run /sbin/init as init process" % tti_b)
    if last_ts:
        lines.append("[  %s] still restoring" % last_ts)
    return "\r\n".join(lines) + "\r\n"


class Fixture:
    """A fake repo: <root>/kho-bench/results/<ds>/{data.csv,raw/*.log}."""

    def __init__(self, root):
        self.root = root

    def dataset(self, ds, runs):
        base = os.path.join(self.root, "kho-bench", "results", ds)
        os.makedirs(os.path.join(base, "raw"), exist_ok=True)
        path = os.path.join(base, "data.csv")
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=COLS)
            w.writeheader()
            for i, r in enumerate(runs):
                log = "raw/%s_%s_%s_run%d.log" % (r["variant"], r["pattern"],
                                                  r["count"], i)
                tti = r.get("tti", "")
                with open(os.path.join(base, log), "w") as lf:
                    lf.write(log_text(r.get("log_tti", tti),
                                      r.get("b_last_ts_s")))
                w.writerow(dict(variant=r["variant"], pattern=r["pattern"],
                                count=r["count"], run=i, time_to_init_s=tti,
                                memblock_reserved_regions=r.get("cnt", ""),
                                logfile=log, status=r.get("status", "ok"),
                                walk1_ns=r.get("walk1_ns", ""),
                                retrieve_ns=r.get("retrieve_ns", ""),
                                b_last_ts_s=r.get("b_last_ts_s", ""),
                                b_timeout_s="1500", keys=""))
        return path


def m2_runs():
    runs = []
    for v in ("baseline", "early"):
        for t in ("1.800000", "1.900000", "1.850000"):
            runs.append(dict(variant=v, pattern="none", count=0, tti=t))
        for t in ("2.000000", "4.000000", "3.000000", "5.000000"):
            runs.append(dict(variant=v, pattern="contig", count=1000, tti=t,
                             cnt="60", walk1_ns="1000000",
                             retrieve_ns="2000000"))
        for t in ("10.000000", "12.000000", "11.000000"):
            runs.append(dict(variant=v, pattern="inverted", count=1000,
                             tti=t, cnt="1000", walk1_ns="9000000",
                             retrieve_ns="9000000"))
    # flagged: measured TTI but b_ssh_timeout
    runs.append(dict(variant="baseline", pattern="inverted", count=4000,
                     tti="1526.863195", status="b_ssh_timeout",
                     b_last_ts_s="1527.934926"))
    # timeout without init line: lower bound only
    runs.append(dict(variant="early", pattern="inverted", count=4000,
                     tti="", status="b_ssh_timeout",
                     b_last_ts_s="1490.500000"))
    # failed generation: no TTI, no lower bound
    runs.append(dict(variant="early", pattern="contig", count=4000, tti="",
                     status="generate_failed(rc=1)"))
    return runs


def read_csv(p):
    with open(p, newline="") as f:
        return list(csv.DictReader(f))


class DataTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(self.tmp.name)
        self.csv = self.fx.dataset("m2", m2_runs())

    def tearDown(self):
        self.tmp.cleanup()

    def test_classify_each_row_kind(self):
        rows, _ = kd.load([self.csv], self.tmp.name)
        cls = {(r["variant"], r["pattern"], r["count"]): r["_class"]
               for r in rows if r["count"] == "4000"}
        self.assertEqual(cls[("baseline", "inverted", "4000")], "flagged")
        self.assertEqual(cls[("early", "inverted", "4000")], "lower_bound")
        self.assertEqual(cls[("early", "contig", "4000")], "failed")

    def test_log_paths_are_repo_relative(self):
        rows, info = kd.load([self.csv], self.tmp.name)
        self.assertTrue(rows[0]["_log"].startswith(
            "kho-bench/results/m2/raw/"))
        self.assertEqual(info[0]["label"], "m2")
        self.assertEqual(info[0]["path"], "kho-bench/results/m2/data.csv")

    def test_aggregate_median_min_max_use_ok_rows_only(self):
        rows, _ = kd.load([self.csv], self.tmp.name)
        pts = {(p.variant, p.pattern, p.count): p for p in kd.aggregate(rows)}
        c = pts[("baseline", "contig", 1000)]
        self.assertEqual(kd.stats(c.ok_vals), (3.5, 2.0, 5.0))  # even n
        f = pts[("baseline", "inverted", 4000)]
        self.assertEqual(f.ok_vals, [])
        self.assertAlmostEqual(f.flagged[0][0], 1526.863195)
        lb = pts[("early", "inverted", 4000)]
        self.assertEqual(lb.ok_vals, [])
        self.assertAlmostEqual(lb.lower[0][0], 1490.5)
        self.assertEqual(len(pts[("early", "contig", 4000)].failed), 1)

    def test_log_scan_uses_kernel_b_section_only(self):
        rows, _ = kd.load([self.csv], self.tmp.name)
        s = kd.log_scan(rows[0]["_log_abs"])
        self.assertEqual(s["tti"], "1.800000")   # not kernel A's 1.000000
        self.assertEqual(s["mem_kb"], 2000)
        self.assertEqual(s["cpus"], 8)

    def test_verify_logs_detects_csv_log_mismatch(self):
        runs = m2_runs()
        runs[0]["log_tti"] = "9.999999"          # log disagrees with csv
        p = self.fx.dataset("bad", runs)
        rows, _ = kd.load([p], self.tmp.name)
        probs, _ = kd.verify_logs(rows)
        self.assertEqual(len(probs), 1)
        self.assertIn("9.999999", probs[0])

    def test_filter_drops_rows(self):
        rows, info = kd.load([self.csv], self.tmp.name, ["variant=early"])
        self.assertTrue(all(r["variant"] == "early" for r in rows))
        self.assertEqual(info[0]["n_used"], len(rows))


class EndToEndTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.fx = Fixture(self.root)
        self.m2 = self.fx.dataset("m2", m2_runs())
        self.out = os.path.join(self.root, "graphs")
        self.report = os.path.join(self.root, "REPORT.md")

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *extra):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            kho_plot.main(["--data", self.m2, "--out", self.out,
                           "--repo-root", self.root, "--report", self.report]
                          + list(extra))
        return buf.getvalue()

    def test_every_png_has_csv_of_plotted_points(self):
        self.run_main()
        for name in ("tti_vs_count", "tti_vs_count_by_pattern",
                     "restore_phases_vs_count", "reserved_cnt_vs_count"):
            self.assertTrue(os.path.getsize(
                os.path.join(self.out, name + ".png")) > 1000)
            rows = read_csv(os.path.join(self.out, name + ".csv"))
            self.assertTrue(rows)
            for r in rows:
                self.assertTrue(r["raw_logs"], r)
                for log in r["raw_logs"].split():
                    self.assertTrue(os.path.exists(
                        os.path.join(self.root, log)), log)

    def test_tti_csv_kinds_and_values(self):
        self.run_main()
        rows = read_csv(os.path.join(self.out, "tti_vs_count.csv"))
        by = {(r["mode"], r["pattern"], r["count"], r["point_kind"]): r
              for r in rows}
        med = by[("baseline", "contig", "1000", "median")]
        self.assertEqual((med["median"], med["min"], med["max"],
                          med["n_runs"]),
                         ("3.500000", "2.000000", "5.000000", "4"))
        self.assertEqual(len(med["raw_logs"].split()), 4)
        fl = by[("baseline", "inverted", "4000", "flagged_measured")]
        self.assertEqual(fl["plotted_value"], "1526.863195")
        self.assertIn("b_ssh_timeout", fl["flags"])
        lb = by[("early", "inverted", "4000", "lower_bound")]
        self.assertEqual(lb["plotted_value"], "1490.500000")
        self.assertEqual(lb["median"], "")
        fr = by[("early", "none", "0", "floor_reference_line")]
        self.assertEqual(fr["median"], "1.850000")
        # failed generation is never drawn as a timing: only a labelled
        # x-position marker without any value
        fa = [r for r in rows if r["count"] == "4000" and
              r["pattern"] == "contig"]
        self.assertEqual([r["point_kind"] for r in fa], ["failed_no_tti"])
        self.assertEqual((fa[0]["plotted_value"], fa[0]["median"]), ("", ""))

    def test_worst_case_and_report_numbers(self):
        out = self.run_main()
        self.assertIn("worst-case pattern (highest measured TTI): inverted",
                      out)
        with open(os.path.join(self.out, "report_numbers.json")) as f:
            n = json.load(f)
        self.assertEqual(n["worst"]["pattern"], "inverted")
        self.assertEqual(n["rows_used"], len(m2_runs()))
        with open(self.report) as f:
            rep = f.read()
        self.assertNotIn("{{", rep)
        self.assertIn("Pending M3", rep)
        self.assertIn("1526.863", rep)
        self.assertIn("> 1490.500 s", rep)

    def test_refuses_to_plot_when_log_disagrees(self):
        runs = m2_runs()
        runs[3]["log_tti"] = "7.000000"
        self.m2 = self.fx.dataset("m2", runs)
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                self.run_main()

    def test_second_dataset_adds_modes(self):
        m3 = self.fx.dataset("m3", [
            dict(variant="early_bulk", pattern="inverted", count=1000,
                 tti="3.000000"),
            dict(variant="early_bulk", pattern="inverted", count=1000,
                 tti="3.200000"),
            dict(variant="baseline", pattern="inverted", count=1000,
                 tti="11.500000")])
        self.run_main("--data", m3)
        head = read_csv(os.path.join(self.out, "tti_vs_count.csv"))
        self.assertIn("early_bulk", {r["mode"] for r in head})
        rows = read_csv(os.path.join(self.out, "tti_vs_count_by_pattern.csv"))
        modes = {r["mode"] for r in rows}
        # baseline is in both datasets -> qualified by label
        self.assertTrue({"m2/baseline", "m3/baseline", "early",
                         "early_bulk"} <= modes, modes)
        eb = [r for r in rows if r["mode"] == "early_bulk"][0]
        self.assertEqual(eb["median"], "3.100000")
        self.assertEqual(eb["data_csv"], "kho-bench/results/m3/data.csv")
        with open(self.report) as f:
            rep = f.read()
        self.assertNotIn("Pending M3", rep)
        self.assertIn("early_bulk", rep)

    def test_best_mode_and_same_dataset_comparisons(self):
        runs = []
        for v, ttis in (("early_opt", ("2.0", "2.1", "2.2")),
                        ("early_bulk", ("5.0", "5.1", "5.2")),
                        ("baseline", ("9.0", "9.5", "10.0")),
                        ("early", ("8.0", "8.5", "9.0"))):
            for t in ttis:
                runs.append(dict(variant=v, pattern="inverted", count=1000,
                                 tti=t + "00000"))
        m3 = self.fx.dataset("m3", runs)
        out = self.run_main("--data", m3)
        self.assertIn("best mode (lowest median at inverted 1000): "
                      "early_opt", out)
        with open(os.path.join(self.out, "report_numbers.json")) as f:
            n = json.load(f)
        self.assertEqual(n["best"]["mode"], "early_opt")
        # m3 modes are only compared with m3 references, never with m2
        for c in n["compare"]:
            if c["mode"] in ("early_opt", "early_bulk", "m3/early"):
                self.assertTrue(c["ref"].startswith("m3/"), c)
        e = [d for d in n["effects"] if d["mode"] == "early_opt"][0]
        self.assertEqual((e["ref"], e["ref_median"], e["median"]),
                         ("m3/early", 8.5, 2.1))
        self.assertAlmostEqual(e["speedup"], 8.5 / 2.1)
        for name in ("tti_worst_case", "tti_best_mode"):
            rows = read_csv(os.path.join(self.out, name + ".csv"))
            self.assertTrue(rows)
            self.assertTrue(all(r["raw_logs"] for r in rows))
        bars = [r for r in read_csv(os.path.join(
            self.out, "tti_worst_case.csv")) if r["panel"] == "bars@1000"]
        self.assertEqual({r["mode"] for r in bars},
                         {"early_opt", "early_bulk", "m3/baseline",
                          "m3/early"})
        with open(self.report) as f:
            rep = f.read()
        self.assertIn("**Best mode: `early_opt`**", rep)

    def test_modes_filter(self):
        self.run_main("--modes", "early")
        rows = read_csv(os.path.join(self.out, "tti_vs_count.csv"))
        self.assertEqual({r["mode"] for r in rows}, {"early"})


if __name__ == "__main__":
    unittest.main()
