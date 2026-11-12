# kho-bench M3 harness (improvement modes)

Copies of the M2 scripts, adapted for the M3 modes. M1/M2 scripts and results are untouched;
everything here writes to `kho-bench/results/m3/`.

| File | Change vs M2 |
|---|---|
| `run_point.sh` | default output `results/m3/{raw,data.csv}`, `--probe` -> `results/m3/probe/` |
| `kho_parse.py` | same columns/order as M2, M3 columns appended: `opts ranges node_ns count_ns presize_ns alloc_ns collect_ns sort_ns bulk_ns streams walk1_fallback walk2_pages pages_inited nodes_late_free_n nodes_late_free_ns mfa_reserved_pages_ns mfa_free_ranges_ns check`; new statuses `opts_mismatch(x)` (B's `opts=` differs from the mode table, skipped if `kho_reserve_opts=` is on B's cmdline) and `check_failed` (`kho_reserve_check=1` found an inconsistency) |
| `kho_sweep.py` | `VARIANTS` = all 9 modes; plan keywords `both` (baseline+early), `opt` (7 early_* modes), `all`; plan option `bt=<s>` = kernel B timeout for that point; defaults `m3_plan.tsv`, `results/m3` |
| `kho_summary.py` | rewritten generically for any mode: per-mode TTI tables with phase timers, walk1 sub-timers, mode-minus-early and mode-minus-baseline (exact MWU p), worst case per mode with overhead vs the mode's none/0 floor, verify table, raw-log re-parse check -> `summary.md`, `summary.csv` |
| `kho_plot.py` | new: `graphs/tti_vs_count_<pattern>`, `overhead_vs_floor_<pattern>`, `components_<pattern>_<count>` (PNG+SVG) and `graphs/plot_points.csv` (every plotted value with its raw logs) |
| `m3_plan.tsv`, `m3_sweep.sh` | M3 plan (phase 1 early_opt sweeps, 2 baseline/early on the same image, 3 single modes, 4 large verify) |

Modes (kernel B `kho_reserve_mode=`), bits printed as `KHO_BENCH: mode=<m> opts=0x..`:

| mode | opts | change |
|---|---|---|
| baseline | 0 | upstream flow |
| early | 0 | M1 prototype |
| early_noinit1 | 0x1 | one `__memblock_reserve(..., MEMBLOCK_RSRV_NOINIT)` per key |
| early_coalesce | 0x2 | whole-bitmap walk, one reserve per contiguous run |
| early_presize | 0x4 | count pass, grow `memblock.reserved` once |
| early_bulk | 0x8 | count, presize, collect runs + node pages, per-stream linear merge (`memblock_reserve_bulk()`) |
| early_nodefree | 0x10 | node pages stay reserved, freed by `free_reserved_page()` in a core_initcall |
| early_batchinit | 0x20 | walk 2 uses `init_deferred_page_range()` |
| early_opt | 0x3f | all of the above |

Debug knobs (A cmdline via `--extra-cmdline`, inherited by B): `kho_reserve_check=1` (verify
memblock.reserved after walk 1, prints `KHO_BENCH: mode=.. check ok|FAILED ...`),
`kho_reserve_opts=<n>` (override the mode bits; `0x100` pretends the temporary allocations
fail to exercise the fallback to the per-key walk).

Usage:
```
kho-bench/scripts/m3/run_point.sh --verify --extra-cmdline kho_reserve_check=1 early_opt alt 1048576 1 fill=1
kho-bench/scripts/m3/m3_sweep.sh --dry-run
setsid nohup kho-bench/scripts/m3/m3_sweep.sh </dev/null >>kho-bench/results/m3/sweep.out 2>&1 &
python3 kho-bench/scripts/m3/kho_summary.py   # -> results/m3/summary.md, summary.csv
python3 kho-bench/scripts/m3/kho_plot.py      # -> results/m3/graphs/
```
`results/m3/smoke/` holds the implementation smoke/verify boots (single runs, not sweep data).
