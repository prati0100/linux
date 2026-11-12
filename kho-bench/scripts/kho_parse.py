#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Parse a KHO bench serial log (kernel A cold boot + kernel B kexec boot).

Usage:
  kho_parse.py show <serial.log>             # print parsed fields
  kho_parse.py row --csv data.csv --log L --variant V --pattern P --count N
                   --run K [--status S] [--verify V] [--params "..."]
                   [--b-hugetlb-kb X] [--b-memfree-kb X] [--a-memfree-kb X]
                   [--git-desc D]
      parse L and append one CSV row (header written if CSV is new).

Everything here is derived from the log text; nothing is invented.  Fields
that are not found are left empty and the status column says why.
"""
import argparse
import csv
import fcntl
import os
import re
import sys
import time

BASE_COLS = ["variant", "pattern", "count", "run", "time_to_init_s",
             "memblock_reserved_regions", "kernel_git_desc", "timestamp",
             "logfile"]
EXTRA_COLS = ["status", "mode_seen", "keys", "nodes", "reserved_before",
              "reserved_max", "walk1_ns", "walk2_ns", "free_nodes_ns",
              "discover_ns", "retrieve_ns", "memblock_free_all_ns",
              "gen_ms", "gen_pages", "gen_chunks", "found_pages",
              "b_hugepages_1g_boot", "b_hugetlb_kb", "b_memfree_kb",
              "a_memfree_kb", "a_warnings", "b_warnings", "verify",
              "kernel_build", "params",
              # M2 additions (appended so that column 10 stays "status")
              "b_last_ts_s", "b_dmesg_init_s", "b_ssh_s", "b_timeout_s",
              "bzimage_sha", "b_cmdline_mode", "b_cmdline"]
COLS = BASE_COLS + EXTRA_COLS

# Not anchored: userspace console output can land in front of a kernel
# printk on the same serial line (e.g. "Failed [    1.53] Run /sbin/init").
TS = r"\[\s*(\d+\.\d+)\]"
RE_LINUX = re.compile(TS + r" Linux version (\S+) .*?(#\d+ .*)$")
RE_INIT = re.compile(TS + r" Run /sbin/init as init process")
RE_INIT_ANY = re.compile(r"Run /sbin/init as init process")
RE_TS_ANY = re.compile(TS)
RE_CMDLINE = re.compile(TS + r" Command line: (.*)$")
RE_MODE = re.compile(r"KHO_BENCH: mode=(\S+)")
RE_WALK1 = re.compile(r"KHO_BENCH: mode=\S+ walk1 keys=(\d+) nodes=(\d+) "
                      r"bytes=(\d+) reserved\.cnt=(\d+)->(\d+) max=(\d+) "
                      r"t=(\d+)ns")
RE_WALK2 = re.compile(r"KHO_BENCH: mode=\S+ walk2 keys=(\d+) .*t=(\d+)ns "
                      r"free_nodes_t=(\d+)ns")
RE_DISC = re.compile(r"KHO_BENCH: mode=\S+ discover t=(\d+)ns")
RE_RETR = re.compile(r"KHO_BENCH: mode=\S+ retrieve reserved\.cnt=(\d+)->"
                     r"(\d+) max=(\d+) t=(\d+)ns")
RE_MBFA = re.compile(r"KHO_BENCH: memblock_free_all t=(\d+)ns")
RE_GEN = re.compile(r"kho_bench: generated pattern=(\S+) count=(\d+) "
                    r"chunks=(\d+) pages=(\d+) .* t=(\d+)ms")
RE_GENFAIL = re.compile(r"kho_bench: generate pattern=\S+ count=\d+ failed")
RE_FOUND = re.compile(r"kho_bench: found pattern=(\S+) count=(\d+) "
                      r"pages=(\d+)")
RE_VERIFY = re.compile(r"kho_bench: VERIFY (PASS|FAIL)")
RE_HUGE1G = re.compile(r"HugeTLB: registered 1\.00 GiB page size, "
                       r"pre-allocated (\d+) pages")
RE_EXEC = re.compile(r"KHO: exec from:")
RE_BAD = re.compile(r"WARNING:|BUG:|Oops|Kernel panic|KHO restore failed")


def strip(line):
    return line.replace("\r", "").rstrip("\n")


def parse(path):
    with open(path, errors="replace") as f:
        lines = [strip(l) for l in f]
    # Split into kernel sections at each "Linux version" banner.
    starts = [i for i, l in enumerate(lines) if RE_LINUX.search(l)]
    r = {"n_kernels": len(starts)}
    if not starts:
        return r
    a = lines[starts[0]:starts[1] if len(starts) > 1 else len(lines)]
    b = lines[starts[1]:starts[2] if len(starts) > 2 else len(lines)] \
        if len(starts) > 1 else []

    def first(regex, sec):
        for l in sec:
            m = regex.search(l)
            if m:
                return m
        return None

    def last(regex, sec):
        found = None
        for l in sec:
            m = regex.search(l)
            if m:
                found = m
        return found

    m = first(RE_GEN, a)
    if m:
        r.update(gen_ms=m.group(5), gen_pages=m.group(4),
                 gen_chunks=m.group(3))
    r["a_gen_failed"] = bool(first(RE_GENFAIL, a))
    r["a_warnings"] = sum(1 for l in a if RE_BAD.search(l))
    m = last(RE_INIT, a)
    r["a_init_s"] = m.group(1) if m else ""
    if not b:
        return r
    m = RE_LINUX.search(b[0])
    r["kernel_build"] = m.group(3).strip() if m else ""
    m = last(RE_INIT, b)
    r["time_to_init_s"] = m.group(1) if m else ""
    r["b_init_line_any"] = bool(first(RE_INIT_ANY, b))
    m = last(RE_TS_ANY, b)
    r["b_last_ts_s"] = m.group(1) if m else ""
    m = first(RE_CMDLINE, b)
    r["b_cmdline"] = m.group(2).strip() if m else ""
    m = first(RE_MODE, b)
    r["mode_seen"] = m.group(1) if m else ""
    m = first(RE_WALK1, b)
    if m:
        r.update(keys=m.group(1), nodes=m.group(2),
                 reserved_before=m.group(4),
                 memblock_reserved_regions=m.group(5),
                 reserved_max=m.group(6), walk1_ns=m.group(7))
    m = first(RE_WALK2, b)
    if m:
        r.update(walk2_ns=m.group(2), free_nodes_ns=m.group(3))
    m = first(RE_DISC, b)
    if m:
        r["discover_ns"] = m.group(1)
    m = first(RE_RETR, b)
    if m:
        r.update(reserved_before=m.group(1),
                 memblock_reserved_regions=m.group(2),
                 reserved_max=m.group(3), retrieve_ns=m.group(4))
    m = first(RE_MBFA, b)
    if m:
        r["memblock_free_all_ns"] = m.group(1)
    m = first(RE_FOUND, b)
    if m:
        r["found_pages"] = m.group(3)
    m = first(RE_HUGE1G, b)
    r["b_hugepages_1g_boot"] = m.group(1) if m else ""
    r["b_exec_from"] = bool(first(RE_EXEC, b))
    r["b_walk1_failed"] = any("walk1 FAILED" in l or "falling back" in l
                              for l in b)
    r["b_warnings"] = sum(1 for l in b if RE_BAD.search(l))
    m = last(RE_VERIFY, b)
    r["verify_log"] = m.group(1) if m else ""
    return r


def log_status(r, variant):
    """Problems derivable from the log alone."""
    probs = []
    if r.get("n_kernels", 0) < 2:
        probs.append("no_kernel_b")
    elif r["n_kernels"] > 2:
        probs.append("extra_boot")
    if r.get("a_gen_failed"):
        probs.append("generate_failed")
    if r.get("n_kernels", 0) >= 2:
        if not r.get("time_to_init_s"):
            probs.append("init_line_garbled" if r.get("b_init_line_any")
                         else "b_no_init")
        if not r.get("b_exec_from"):
            probs.append("b_no_kho")
        if r.get("mode_seen") != variant:
            probs.append("mode_mismatch(%s)" % r.get("mode_seen", ""))
        if r.get("b_walk1_failed"):
            probs.append("walk1_failed_fallback")
        if r.get("b_warnings"):
            probs.append("b_warn_or_bug")
    return probs


def cmd_row(a):
    r = parse(a.log) if os.path.exists(a.log) else {"n_kernels": 0}
    probs = [p for p in (a.status or "").split(";") if p and p != "ok"]
    for p in log_status(r, a.variant):
        if p not in probs:
            probs.append(p)
    if a.verify == "FAIL":
        probs.append("verify_fail")
    if a.b_cmdline_mode == "nohp" and r.get("n_kernels", 0) >= 2:
        # B must not pre-allocate boot-time 1G hugepages (M1 confound).
        if "hugepages" in r.get("b_cmdline", ""):
            probs.append("b_cmdline_has_hugepages")
        if r.get("b_hugepages_1g_boot") != "0":
            probs.append("b_hugepages_nonzero(%s)" %
                         r.get("b_hugepages_1g_boot", ""))
    # A run without the B init timestamp cannot carry a time value.
    tti = r.get("time_to_init_s", "") if "no_kernel_b" not in probs else ""
    if a.b_dmesg_init and tti:
        # Cross-check the serial value against B's own dmesg (not
        # interleaved with userspace console output).
        m = RE_INIT.search(a.b_dmesg_init)
        if not m or abs(float(m.group(1)) - float(tti)) > 0.001:
            probs.append("tti_mismatch")
    dm = RE_INIT.search(a.b_dmesg_init or "")
    row = {c: "" for c in COLS}
    row.update({k: v for k, v in r.items() if k in COLS})
    row.update(variant=a.variant, pattern=a.pattern, count=a.count,
               run=a.run, time_to_init_s=tti,
               kernel_git_desc=a.git_desc,
               timestamp=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               logfile=os.path.relpath(a.log, os.path.dirname(
                   os.path.abspath(a.csv))),
               status="ok" if not probs else ";".join(probs),
               verify=a.verify or r.get("verify_log", "") or "skipped",
               b_hugetlb_kb=a.b_hugetlb_kb or "",
               b_memfree_kb=a.b_memfree_kb or "",
               a_memfree_kb=a.a_memfree_kb or "",
               params=a.params or "",
               b_dmesg_init_s=dm.group(1) if dm else "",
               b_ssh_s=a.b_ssh_s or "",
               b_timeout_s=a.b_timeout or "",
               bzimage_sha=a.bzimage_sha or "",
               b_cmdline_mode=a.b_cmdline_mode or "")
    new = not os.path.exists(a.csv) or os.path.getsize(a.csv) == 0
    with open(a.csv, "a", newline="") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        w = csv.DictWriter(f, fieldnames=COLS, lineterminator="\n")
        if new:
            w.writeheader()
        w.writerow(row)
    print(",".join(str(row[c]) for c in COLS))
    return 0


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("log")
    w = sub.add_parser("row")
    for opt in ["csv", "log", "variant", "pattern", "count", "run"]:
        w.add_argument("--" + opt, required=True)
    for opt in ["status", "verify", "params", "b-hugetlb-kb", "b-memfree-kb",
                "a-memfree-kb", "git-desc", "b-dmesg-init", "b-ssh-s",
                "b-timeout", "bzimage-sha", "b-cmdline-mode"]:
        w.add_argument("--" + opt, default="")
    a = p.parse_args()
    if a.cmd == "show":
        r = parse(a.log)
        for k in sorted(r):
            print("%s=%s" % (k, r[k]))
        return 0
    return cmd_row(a)


if __name__ == "__main__":
    sys.exit(main())
