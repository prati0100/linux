# kho-bench: KHO early-reserve benchmark harness

These are the measurement scripts for the KHO "early memblock reserve" prototype
(`kho_reserve_mode=` in kernel B, generator `lib/test_kho_bench.c` in kernel A).
Every number comes from a real VM boot using the user's `mk r` (`~/bin/qemu.sh`).

## Layout
- `scripts/run_point.sh`: runs one benchmark point, `runs` times. Each run is a cold boot of
  kernel A, then generate, then kexec into kernel B, then (optionally) verify, then poweroff.
- `scripts/probe_max.sh <variant> <pattern>:<start>[:<cap>] ...`: capacity probe. It doubles
  the count per pattern until a run fails, and writes the results to `results/probe/`.
- `scripts/kho_parse.py`: parses a serial log (`show`) and appends a CSV row (`row`).
  You can re-run it on any raw log.
- `results/raw/<variant>_<pattern>_<count>_run<k>.log`: the full serial console of A and B
  (qemu stdout, plus a final `time ...` line from `mk`).
- `results/raw/<...>_run<k>.harness.log`: the harness side log. It holds the ssh outputs:
  meminfo before and after freeing the 1G hugepages, kho_bench status, B dmesg excerpt,
  verify line, and the bzImage sha256.
- `results/data.csv`: one row per measured run.
- `results/probe/`: capacity probes and investigations (`--probe`, `--no-kexec`), with their
  own `probe.csv`. These rows are kept out of `data.csv`.
- `patches/m1.patch`: the kernel diff (`git diff` + new `lib/test_kho_bench.c`).
- `patches/m2.patch`: the same for M2 (only change vs m1: lib/Kconfig.debug help-text fix).
- M2 sweep (`scripts/m2_sweep.sh` -> `scripts/kho_sweep.py`, plan `scripts/m2_plan.tsv`),
  summary generator `scripts/kho_summary.py`. Output in `results/m2/`:
  `data.csv` + `raw/` (TTI rows, default params), `verify/{data.csv,raw/}` (fill=1 verify runs),
  `smoke/` (harness smoke boots, not data), `sweep.log` (append-only progress),
  `sweep.status` (one-line heartbeat), `summary.md` + `summary.csv` (generated).

## Usage
Build once with `mk` (the harness never builds). Then, from anywhere:
```
kho-bench/scripts/run_point.sh [--verify] [--no-free-hugepages] [--extra-cmdline "S"]
                               [--b-cmdline nohp|reuse] [--outdir DIR]
                               [--probe] [--no-kexec] [--stop-on-fail]
                               <variant> <pattern> <count> <runs> [order=|stride=|max_order=|seed=|fill= ...]
# examples
kho-bench/scripts/run_point.sh --verify early inverted 1000 1
kho-bench/scripts/run_point.sh baseline alt 100000 5
kho-bench/scripts/run_point.sh --probe early mixed 65536 1      # capacity probe
kho-bench/scripts/run_point.sh --no-kexec x none 0 1             # boot A + free hugepages only
```
- `variant` is passed to B as `kho_reserve_mode=<variant>`. With `--b-cmdline nohp` (default
  since M2) the harness reads A's `/proc/cmdline`, removes every `hugepages=`, `hugepagesz=` and
  `default_hugepagesz=` param, appends `kho_reserve_mode=<variant>` and loads B with
  `kexec -s -l /root/bzImage --command-line='...'` (logged as `B cmdline:` in the harness log).
  The row gets `b_hugepages_nonzero(N)` / `b_cmdline_has_hugepages` if B still pre-allocates
  1G pages. `--b-cmdline reuse` is the M1 behaviour (`--reuse-cmdline --append=...`; B inherits
  `hugepages=50`).
- `--outdir DIR`: logs go to `DIR/raw/`, rows to `DIR/data.csv` (default `results/raw` +
  `results/data.csv`).
- Exit status: 0 all runs ok, 1 some run failed, 2 usage error, 3 refused (VM lock held or a
  qemu-system-x86 already running; nothing was run).
  B must print `KHO_BENCH: mode=<variant>`, otherwise the row gets the status `mode_mismatch`.
- `pattern`/`count`/`k=v` go on A's cmdline as `kho_bench.<k>=<v>` (B inherits them, which is
  harmless because B only reads the handover metadata). They are also written to
  `/sys/module/kho_bench/parameters/` before `generate`.
- In A, the harness frees the 50 boot-time 1G hugepages with `nr_hugepages=0` (pass
  `--no-free-hugepages` to keep them), because `hugepages=0` on the cmdline is ignored. This
  takes MemFree from about 13 GB to about 65 GB.
- `--verify` runs `echo 1 > /sys/kernel/debug/kho_bench/verify` in B after B is reachable over
  ssh. That is long after `Run /sbin/init`, so it does not affect time-to-init.
- Timeouts can be set through the environment (seconds): `A_SSH_TIMEOUT=300 GEN_TIMEOUT=900
  KEXEC_LOAD_TIMEOUT=180 B_TIMEOUT=1500 VERIFY_TIMEOUT=900 POWEROFF_TIMEOUT=120`. The outer
  `timeout` on `mk r` is their sum plus 300 s.
- Single VM: the harness takes `flock -n /tmp/kho-bench-vm.lock` (override with
  `KHO_BENCH_LOCK`) and refuses to start if any `qemu-system-x86` is already running. On
  failure, a timeout or Ctrl-C it first tries `ssh vm poweroff`. If that doesn't work, it kills
  only the qemu in the session it started (`setsid`).
- Logs are never overwritten. The run index `k` is the first index whose `.log` doesn't exist
  yet, and it is claimed atomically (bash `noclobber`). If you re-run a point, you get `run2`,
  `run3`, and so on.
- `/root/bzImage` in the VM (used by kexec) is refreshed with `mk cp` only when its sha256
  differs from `arch/x86/boot/bzImage`. A always cold-boots `arch/x86/boot/bzImage`.

## CSV columns
Contract columns: `variant,pattern,count,run,time_to_init_s,memblock_reserved_regions,kernel_git_desc,timestamp,logfile`.
After those come the extras: `status,mode_seen,keys,nodes,reserved_before,reserved_max,walk1_ns,walk2_ns,free_nodes_ns,discover_ns,retrieve_ns,memblock_free_all_ns,gen_ms,gen_pages,gen_chunks,found_pages,b_hugepages_1g_boot,b_hugetlb_kb,b_memfree_kb,a_memfree_kb,a_warnings,b_warnings,verify,kernel_build,params`.

- `time_to_init_s`: printk timestamp of the last `Run /sbin/init as init process` in the
  kernel-B section of the serial log (B is everything after the 2nd `Linux version` banner).
  It is empty if B never got that far.
- `memblock_reserved_regions`: `memblock.reserved.cnt` right after the KHO reservations in B
  (early: `walk1 reserved.cnt=a->b`; baseline: `retrieve reserved.cnt=a->b`). `reserved_before`
  is `a`.
- `logfile`: relative to `results/`.
- `status`: `ok` or a `;`-separated list of problems, for example `a_ssh_timeout`,
  `generate_failed(rc=N)`, `kexec_load_failed`, `b_ssh_timeout`, `qemu_exited_before_b`,
  `b_no_init`, `b_no_kho`, `mode_mismatch(x)`, `walk1_failed_fallback`, `b_warn_or_bug`,
  `verify_fail`, `verify_no_result`, `qemu_not_stopped`. Failed runs are kept as rows, not dropped.
- `a_warnings`: count of `WARNING:|BUG:|Oops|Kernel panic` lines in A. Each A shows 1:
  `alloc_tag was not set` (alloc_tag.h:166) when the boot-time 1G hugepages are freed. It
  appears even with `kho=0` and without generation (`results/probe/none_none_0_run1*`,
  `results/probe/nokho_none_0_run1*`), so it comes from MEM_ALLOC_PROFILING_DEBUG + hugetlb
  bootmem and not from this prototype.
- `verify`: `PASS`, `FAIL`, `NONE`, or `skipped`.
- M2 columns (appended): `b_last_ts_s` (last printk timestamp in B; lower bound for timeouts),
  `b_dmesg_init_s` (TTI from B's own dmesg; status `tti_mismatch` if it differs by >1 ms from
  the serial value), `b_ssh_s` (host seconds from `kexec -e` to B ssh), `b_timeout_s`,
  `bzimage_sha`, `b_cmdline_mode` (nohp|reuse), `b_cmdline` (B's `Command line:`).
- M2 statuses: `hugepage_free_incomplete` (A's nr_1G != 0 after the free), `b_cmdline_bad`,
  `init_line_garbled` (a `Run /sbin/init` line without a parsable timestamp).
- The timestamp regex is unanchored (since M2): a serial line like
  `Failed [    1.539717] Run /sbin/init as init process` (userspace text in front of a printk)
  is parsed correctly. Re-parsing all M1 logs changes only that one log.

## M2 sweep
```
kho-bench/scripts/m2_sweep.sh --dry-run          # remaining runs per point
setsid nohup kho-bench/scripts/m2_sweep.sh </dev/null >>kho-bench/results/m2/sweep.out 2>&1 &
touch kho-bench/results/m2/STOP                   # graceful stop after the current run
python3 kho-bench/scripts/kho_summary.py          # -> results/m2/summary.md, summary.csv
```
The sweep recomputes its state from `data.csv` before each run (rows with the current bzImage
sha and `b_cmdline_mode=nohp`), so re-running it resumes. One run per point per round, so
variants and patterns interleave. Auto-caps (measured facts only): ENOMEM in A at count N skips
counts >= N of that pattern; a B timeout (or TTI > 600 s) ends that point and reduces larger
counts to one attempt; points with median TTI > 60 s need 3 ok runs instead of 5; more than 2
failed rows at a point -> GIVEUP. Timeouts are recorded as lower bounds, never extrapolated.
