# KHO early-reserve benchmark: M3 summary (improvement modes)

Generated 2026-11-11T14:07:27Z by scripts/m3/kho_summary.py from results/m3 (bzImage sha 5e3c8d47e5c6, 452 TTI rows, 18 verify rows). All values are measured VM boots; medians of status=ok runs, [min,max], (n). Timeouts are lower bounds, never extrapolated.

Raw-log re-check: 470 rows re-parsed, 0 mismatches

## Interpretation (hand-written; every number is cited from the script-generated tables below)

Build #365 (bzImage sha 5e3c8d47e5c6), one image for all 9 modes, kernel B booted without hugepages (`nohp`). The sweep ran 2026-11-11 10:17Z–14:04Z through `scripts/m3/m3_sweep.sh`, one VM at a time via `mk r`. It produced 452 TTI rows and 18 fill=1 verify rows. Every TTI row is status=ok with b_warnings=0. One row is generate_failed (ENOMEM in kernel A, inverted 8M). There are no timeouts and no walk1 fallbacks.

### Verdict
With all improvements combined (**early_opt**), the early-reservation idea is workable in this VM, including the worst case:
- **Worst measured case:** early_opt **inverted 4194304: 4.082 s** median (n=3, 4.023–4.103). That is +2.209 s over the early_opt none/0 floor of 1.873 s (n=5). At 2097152 it is 2.969 s (n=5), +1.096 s over the floor.
- **Plain early on the same pattern** is quadratic. Measured in this session, inverted 524288 takes 71.100 s for early and 71.705 s for baseline (n=1 each, measured before the plan was trimmed). M2's n=3 values are 72.077 / 71.214 s. early_opt takes **2.135 s** (n=5) at that count, which is 0.030× early.
- **Larger counts, M2 only:** for plain early and baseline at inverted ≥1M, only M2 data exists (a different session on build #360, same harness):
  - 1M: 284.940 s early / 281.873 s baseline (n=3).
  - 2M: 1457.235 s early (n=1); baseline 1526.863 s (n=1, flagged b_ssh_timeout).
  - Same-session comparisons in this run that match M2 closely: alt 4M baseline 4.850 vs M2 4.846 s, early 9.420 vs M2 9.499 s; random 1M baseline 3.165 vs 3.161 s, early 3.886 vs 3.873 s; contig 8M early 2.969 vs 3.007 s, baseline 3.909 vs 3.906 s.
- **early_opt is below baseline** at every same-session comparison point with ≥ 32768 blocks. The seven points with n ≥ 3 on both sides have MWU p ≤ 0.036. At none/0 (+0.025 s, p=0.393) and inverted 10000 (+0.027 s, baseline n=1) the difference is within noise:

  | point | early_opt | baseline |
  |---|---|---|
  | contig 8M | 1.997 s | 3.909 s |
  | alt 4M | 4.034 s | 4.850 s |
  | random 1M | 2.964 s | 3.165 s |
  | mixed 32768 | 1.807 s | 2.057 s |
  | inverted 100000 | 1.930 s | 4.106 s |
  | inverted 262144 | 2.005 s | 19.204 s |
  | inverted 524288 | 2.135 s | 71.705 s |

  See the "mode minus baseline" table for MWU p.
- **What remains of early_opt's overhead** at inverted 4M is mostly `memblock_free_all`: 1599.8 ms against 170.2 ms at the floor. That is about 0.34 µs per extra reserved region ((1599.8 − 170.2) ms / ~4.19M), the cost of having ~4.2M regions in memblock. Baseline pays the same: alt 4M mfa is 1625 ms for baseline and 1626 ms for early_opt. walk1 is 243 ms and walk2 130 ms; late node free is 0.13 ms.

### What each single improvement does (same-session medians)
| mode | effect |
|---|---|
| early_bulk (sorted bulk build) | **Removes the O(N²).** inverted 262144 goes 19.292 → 2.051 s and 524288 goes 71.100 → 2.262 s (walk1 27.4 ms). By itself it still pays the radix node free against N regions (free_nodes_t O(nodes×N)): inverted 4M 8.701 s, of which free_nodes is 4273.6 ms; alt 4M 7.597 s, free_nodes 3336.5 ms. |
| early_nodefree | Removes free_nodes. alt 4M goes 9.420 → 6.117 s. random 1M goes 3.886 → 3.365 s. walk1 is still quadratic on inverted (262144: 19.393 s). |
| early_coalesce | Large gain only where runs exist. contig 8M goes 2.969 → 2.400 s, with walk1 555.8 → 0.22 ms. No gain on alt, random or inverted, where every run has length 1 (alt 4M: 9.390 s). |
| early_noinit1 | Small walk1 gain: alt 4M walk1 2055 → 1795 ms, TTI 9.420 → 9.144 s. |
| early_presize | No measurable TTI gain (alt 4M 9.414 s). Doubling was never the cost. |
| early_batchinit | Cuts walk2: alt 4M 349 → 112 ms, TTI 9.286 s. |
| early_opt (all) | The combination: bulk fixes the order problem, nodefree removes the node free cost, batchinit cuts walk2 and coalesce helps contig. |

### Costs and caveats the reviewers asked to state
- **Deferred struct page init is not active in this VM.** CONFIG_MEM_ALLOC_PROFILING_DEBUG=y makes `defer_init()` return false, so `pages_inited=0` in every walk2 line. walk2 in every mode does only the per-page `early_page_initialised()` check, and batchinit's gain is that this per-page check loop is gone. The walk2 numbers (and batchinit's gain) do not carry over to configurations where deferred init is on. Those would also do real struct page init in walk2, and batchinit's inner init loop is untested by measurement.
- **Extra KHO scratch memory.** The bulk/opt modes allocate a temporary range array while the noprsrv-only (scratch-only) restriction is on, and free it after walk1. Its size is page-aligned (ranges + nodes) × 24 B: **96 MiB at 4M blocks** (alt 4M, inverted 4M) and 48 MiB at 2M; see the walk1 sub-timers table.
  - At the same time, memblock.reserved itself needs reserved.max × 24 B: **192 MiB at 4M blocks** (max 8,389,3xx).
  - Peak scratch use while walk1 runs is therefore about **288 MiB at 4M blocks**.
  - The reserved array has the same size without presize (early, baseline: max 8,388,608 at alt 4M). The array growth itself is not new; the temp array is the additional cost.
  - These sizes are derived from logged counts × sizeof(struct memblock_region), not measured allocations. Shrinking scratch is the motivation for this idea, so this scratch need counts as a real cost.
- **Bulk merge cost is O(streams × (n + R)).** Each sorted stream (one per order, plus the node-page stream) is merged into the reserved array with a linear pass. Observed stream counts in ok runs are 3–11 (early_opt) and 4–9 (early_bulk), mostly 6–9. sort() was never needed for >64 streams; sort_t ≤ 0.05 ms.
- **early_noinit1 semantics.** `__memblock_reserve(..., MEMBLOCK_RSRV_NOINIT)` treats overlap with an already existing reservation differently from M1's `memblock_reserve` + `memblock_reserved_mark_noinit`. memblock_add_range() only inserts the parts that are not already reserved, so the part of a key range that overlaps an existing reservation keeps the existing region's flags and is not marked NOINIT. mark_noinit sets the flag on the whole range. memblock_add_range() also WARNs when the flags differ (mm/memblock.c ~829), and no such WARN appeared: b_warnings=0 in every row. This is a known latent difference. It never triggered here: smoke runs with `kho_reserve_check=1` all reported `check ok`, and every verify PASSed.
- **Verify rows** (fill=1 full content verify) are in verify/data.csv and are not used in any timing table or graph. All 18 PASS, covering every mode at large counts.
- **TTI below the floor.** early_opt mixed 32768 (1.807 s) and 40960 (1.830 s) are below the none/0 floor (1.873 s). mixed preserves ~23 GiB, so memblock_free_all has less free memory to release (125 ms vs 170 ms). That is less work, not negative overhead.
- **Noise.** This is a nested cloud VM. Single outliers appear (e.g. early_opt random 10000 max 2.125 s vs median 1.879 s), so medians are reported. Rounds were interleaved: each phase runs round-robin, so the runs of a point are spread over the phase.
- **Single node, single memory region above 4 GiB.** NUMA paths, ranges below 4 GiB and multi-zone paths are not exercised.

### Sweep trimming (parent guidance, 10:39Z: ≤ ~5 h total, essential data first)
- **Dropped re-measurements:** slow baseline/early inverted points ≥ 524288 were not re-measured (1M n=3 and the 2M attempt were dropped). The baseline/early/early_noinit1 inverted 524288 rows (n=1) were measured before the trim and are kept. M2 is cited for the rest.
- **Single modes:** the five per-key single modes (noinit1, coalesce, presize, nodefree, batchinit) stop at inverted 262144 and have 3 runs per point. They were not run at inverted < 100000, at alt/random < 100000, or on mixed. early_bulk goes up to inverted 4M.
- **Reduced ladders:** single modes on random only at 262144 and 1M, and coalesce on contig only at 1M and 8M. Baseline/early references are at cheap counts only (inverted 100000/262144, alt 1M/4M, random 1M, contig 8M, mixed 32768), plus a few points measured before the trim.
- **Full ladders:** early_opt has full ladders on all 5 patterns (n=5) plus none/0 floors for every mode.
- **Beyond 2M:** early_opt and early_bulk inverted 4M have n=3. The early_opt inverted 8M attempt failed in kernel A (generate_failed, ENOMEM; the 4M layout already spans pfn 0x140000–0xdffffd of the 64 GiB guest), so 4M is the largest inverted count this VM can generate.
- **Plan files:** the plan file is `m3_plan.tsv` (rev 3 + phase 6). Earlier revisions are in `.agents/teamwork/m3_worker_2/`. The sweep was stopped and resumed 3 times for plan edits, and no data was discarded.

## Row status counts

| mode | rows | ok | b_ssh_timeout | generate_failed (ENOMEM in A) | other | b_warnings>0 |
|---|---|---|---|---|---|---|
| baseline | 39 | 39 | 0 | 0 | 0 | 0 |
| early | 39 | 39 | 0 | 0 | 0 | 0 |
| early_noinit1 | 30 | 30 | 0 | 0 | 0 | 0 |
| early_coalesce | 36 | 36 | 0 | 0 | 0 | 0 |
| early_presize | 29 | 29 | 0 | 0 | 0 | 0 |
| early_bulk | 42 | 42 | 0 | 0 | 0 | 0 |
| early_nodefree | 29 | 29 | 0 | 0 | 0 | 0 |
| early_batchinit | 29 | 29 | 0 | 0 | 0 | 0 |
| early_opt | 197 | 196 | 0 | 1 | 0 | 0 |

Rows that are not status=ok or have kernel-B warnings:

- early_opt inverted 8388608: status=generate_failed(rc=1);no_kernel_b;generate_failed b_warnings= b_last_ts_s= log=raw/early_opt_inverted_8388608_run1.log

## Overview: median TTI (s) per pattern x count x mode

Cell = median of ok runs (n); `>x` = timeout lower bound; empty = not measured.

| pattern | count | baseline | early | early_noinit1 | early_coalesce | early_presize | early_bulk | early_nodefree | early_batchinit | early_opt |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.848 (3) | 1.854 (3) | 1.859 (3) | 1.866 (3) | 1.864 (3) | 1.855 (3) | 1.863 (3) | 1.882 (3) | 1.873 (5) |
| contig | 1000 |  |  |  |  |  |  |  |  | 1.856 (5) |
| contig | 10000 |  |  |  |  |  |  |  |  | 1.856 (5) |
| contig | 100000 |  |  |  |  |  |  |  |  | 1.862 (5) |
| contig | 1048576 |  |  |  | 1.940 (3) |  |  |  |  | 1.887 (5) |
| contig | 4194304 |  |  |  |  |  |  |  |  | 1.930 (5) |
| contig | 8388608 | 3.909 (5) | 2.969 (5) |  | 2.400 (3) |  |  |  |  | 1.997 (5) |
| alt | 1000 |  |  |  |  |  |  |  |  | 1.866 (5) |
| alt | 4096 |  |  |  |  |  |  |  |  | 1.858 (5) |
| alt | 10000 |  |  |  |  |  |  |  |  | 1.868 (5) |
| alt | 32768 |  |  |  |  |  |  |  |  | 1.875 (5) |
| alt | 100000 |  |  | 1.935 (3) | 1.921 (3) | 1.933 (3) | 1.923 (3) | 1.927 (3) | 1.939 (3) | 1.930 (5) |
| alt | 262144 |  |  | 2.050 (3) | 2.051 (3) | 2.081 (3) | 2.005 (3) | 2.034 (3) | 2.039 (3) | 1.997 (5) |
| alt | 1048576 | 2.600 (5) | 2.931 (5) | 2.782 (3) | 2.868 (3) | 2.833 (3) | 2.656 (3) | 2.622 (3) | 2.862 (3) | 2.415 (5) |
| alt | 2097152 |  |  |  |  |  |  |  |  | 2.962 (5) |
| alt | 4194304 | 4.850 (5) | 9.420 (5) | 9.144 (3) | 9.390 (3) | 9.414 (3) | 7.597 (3) | 6.117 (3) | 9.286 (3) | 4.034 (5) |
| random | 1000 |  |  |  |  |  |  |  |  | 1.845 (5) |
| random | 4096 |  |  |  |  |  |  |  |  | 1.863 (5) |
| random | 10000 |  |  |  |  |  |  |  |  | 1.879 (5) |
| random | 32768 |  |  |  |  |  |  |  |  | 1.898 (5) |
| random | 100000 |  |  |  |  |  |  |  |  | 1.971 (5) |
| random | 262144 |  |  | 2.242 (3) | 2.217 (3) | 2.218 (3) | 2.207 (3) | 2.193 (3) | 2.196 (3) | 2.123 (5) |
| random | 1048576 | 3.165 (5) | 3.886 (5) | 3.877 (3) | 3.856 (3) | 3.892 (3) | 3.530 (3) | 3.365 (3) | 3.767 (3) | 2.964 (5) |
| random | 1572864 |  |  |  |  |  |  |  |  | 3.505 (3) |
| mixed | 1000 |  |  |  |  |  |  |  |  | 1.869 (5) |
| mixed | 4096 |  |  |  |  |  |  |  |  | 1.849 (5) |
| mixed | 10000 |  |  |  |  |  |  |  |  | 1.855 (5) |
| mixed | 32768 | 2.057 (5) | 2.127 (5) |  |  |  |  |  |  | 1.807 (5) |
| mixed | 40960 |  |  |  |  |  |  |  |  | 1.830 (5) |
| inverted | 1000 |  |  |  |  |  |  |  |  | 1.866 (5) |
| inverted | 4096 |  |  |  |  |  |  |  |  | 1.869 (5) |
| inverted | 10000 | 1.857 (1) | 1.907 (1) |  |  |  |  |  |  | 1.885 (5) |
| inverted | 32768 | 2.082 (1) | 2.108 (1) |  |  |  | 1.891 (1) |  |  | 1.886 (5) |
| inverted | 100000 | 4.106 (5) | 4.111 (5) | 4.107 (3) | 4.097 (3) | 4.126 (3) | 1.925 (3) | 4.152 (3) | 4.116 (3) | 1.930 (5) |
| inverted | 262144 | 19.204 (3) | 19.292 (3) | 19.200 (3) | 19.247 (3) | 19.287 (3) | 2.051 (3) | 19.393 (3) | 19.321 (3) | 2.005 (5) |
| inverted | 524288 | 71.705 (1) | 71.100 (1) | 70.074 (1) |  |  | 2.262 (3) |  |  | 2.135 (5) |
| inverted | 1048576 |  |  |  |  |  | 2.703 (3) |  |  | 2.405 (5) |
| inverted | 2097152 |  |  |  |  |  | 4.153 (3) |  |  | 2.969 (5) |
| inverted | 4194304 |  |  |  |  |  | 8.701 (3) |  |  | 4.082 (3) |
| inverted | 8388608 |  |  |  |  |  |  |  |  | fail |

## Time to init (s) per mode

### baseline

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.848 [1.828,1.884] (3) | 0.028 | 59 | 0.0 | - | - | - | 169.9 | - |
| contig | 8388608 | 3.909 [3.902,3.996] (5) | 0.040 | 62 | 1427.4 | - | - | - | 84.0 | - |
| alt | 1048576 | 2.600 [2.562,2.648] (5) | 0.036 | 1048636 | 274.8 | - | - | - | 533.5 | - |
| alt | 4194304 | 4.850 [4.837,5.011] (5) | 0.074 | 4194365 | 1125.4 | - | - | - | 1625.2 | - |
| random | 1048576 | 3.165 [3.156,3.226] (5) | 0.028 | 1048638 | 319.1 | - | - | - | 1076.4 | - |
| mixed | 32768 | 2.057 [2.050,2.076] (5) | 0.012 | 32806 | 242.2 | - | - | - | 126.6 | - |
| inverted | 10000 | 1.857 [1.857,1.857] (1) | - | 10060 | 24.4 | - | - | - | 170.9 | - |
| inverted | 32768 | 2.082 [2.082,2.082] (1) | - | 32828 | 241.2 | - | - | - | 179.6 | - |
| inverted | 100000 | 4.106 [4.067,4.129] (5) | 0.023 | 100061 | 2206.3 | - | - | - | 203.8 | - |
| inverted | 262144 | 19.204 [19.080,19.386] (3) | 0.154 | 262205 | 17258.2 | - | - | - | 259.3 | - |
| inverted | 524288 | 71.705 [71.705,71.705] (1) | - | 524351 | 69608.4 | - | - | - | 346.1 | - |

### early

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.854 [1.843,1.886] (3) | 0.022 | 22 | 0.0 | 0.0 | 0.0 | - | 170.9 | - |
| contig | 8388608 | 2.969 [2.962,3.026] (5) | 0.026 | 251 | 555.8 | 659.1 | 0.1 | - | 84.6 | - |
| alt | 1048576 | 2.931 [2.863,2.961] (5) | 0.046 | 1048660 | 228.8 | 86.9 | 278.9 | - | 535.0 | - |
| alt | 4194304 | 9.420 [9.279,9.599] (5) | 0.119 | 4194554 | 2055.5 | 349.0 | 3355.2 | - | 1624.3 | - |
| random | 1048576 | 3.886 [3.814,4.015] (5) | 0.077 | 1048821 | 354.1 | 160.4 | 521.4 | - | 1072.5 | - |
| mixed | 32768 | 2.127 [2.108,2.156] (5) | 0.017 | 33143 | 243.2 | 25.3 | 29.4 | - | 127.8 | - |
| inverted | 10000 | 1.907 [1.907,1.907] (1) | - | 10025 | 23.4 | 1.2 | 0.7 | - | 171.9 | - |
| inverted | 32768 | 2.108 [2.108,2.108] (1) | - | 32791 | 238.5 | 3.8 | 2.2 | - | 186.0 | - |
| inverted | 100000 | 4.111 [4.085,4.164] (5) | 0.029 | 100034 | 2194.3 | 11.4 | 8.0 | - | 200.5 | - |
| inverted | 262144 | 19.292 [19.040,19.532] (3) | 0.246 | 262183 | 17260.6 | 29.7 | 25.6 | - | 259.7 | - |
| inverted | 524288 | 71.100 [71.100,71.100] (1) | - | 524343 | 68867.4 | 58.4 | 60.2 | - | 350.2 | - |

### early_noinit1

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.859 [1.858,1.876] (3) | 0.010 | 21 | 0.0 | 0.0 | 0.0 | - | 169.2 | - |
| alt | 100000 | 1.935 [1.906,1.939] (3) | 0.018 | 100030 | 9.8 | 8.4 | 8.5 | - | 203.4 | - |
| alt | 262144 | 2.050 [2.040,2.068] (3) | 0.014 | 262183 | 30.1 | 21.7 | 25.5 | - | 257.8 | - |
| alt | 1048576 | 2.782 [2.663,2.854] (3) | 0.097 | 1048658 | 172.1 | 86.7 | 216.7 | - | 533.3 | - |
| alt | 4194304 | 9.144 [8.990,9.208] (3) | 0.112 | 4194551 | 1795.3 | 349.7 | 3268.5 | - | 1623.2 | - |
| random | 262144 | 2.242 [2.234,2.492] (3) | 0.147 | 262228 | 38.8 | 40.5 | 37.6 | - | 401.6 | - |
| random | 1048576 | 3.877 [3.735,4.039] (3) | 0.152 | 1048823 | 304.7 | 161.4 | 556.1 | - | 1071.6 | - |
| inverted | 100000 | 4.107 [4.075,4.121] (3) | 0.024 | 100031 | 2204.6 | 11.0 | 7.3 | - | 203.1 | - |
| inverted | 262144 | 19.200 [19.156,19.248] (3) | 0.046 | 262185 | 17179.6 | 29.8 | 27.2 | - | 256.4 | - |
| inverted | 524288 | 70.074 [70.074,70.074] (1) | - | 524344 | 67890.3 | 58.3 | 62.4 | - | 347.3 | - |

### early_coalesce

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.866 [1.863,1.883] (3) | 0.011 | 23 | 0.0 | 0.0 | 0.0 | - | 169.5 | - |
| contig | 1048576 | 1.940 [1.929,1.945] (3) | 0.008 | 54 | 0.1 | 81.4 | 0.0 | - | 158.8 | - |
| contig | 8388608 | 2.400 [2.399,2.424] (3) | 0.014 | 249 | 0.2 | 655.1 | 0.1 | - | 84.6 | - |
| alt | 100000 | 1.921 [1.920,1.924] (3) | 0.002 | 100033 | 12.9 | 8.2 | 8.1 | - | 203.2 | - |
| alt | 262144 | 2.051 [2.051,2.058] (3) | 0.004 | 262188 | 39.5 | 21.5 | 25.3 | - | 257.0 | - |
| alt | 1048576 | 2.868 [2.822,2.888] (3) | 0.034 | 1048659 | 217.8 | 85.8 | 237.0 | - | 531.4 | - |
| alt | 4194304 | 9.390 [9.288,9.418] (3) | 0.069 | 4194555 | 1987.6 | 341.2 | 3299.7 | - | 1623.0 | - |
| random | 262144 | 2.217 [2.199,2.242] (3) | 0.022 | 262226 | 47.0 | 40.3 | 37.6 | - | 393.4 | - |
| random | 1048576 | 3.856 [3.773,4.015] (3) | 0.123 | 1048819 | 334.0 | 160.0 | 534.7 | - | 1070.3 | - |
| inverted | 100000 | 4.097 [4.089,4.111] (3) | 0.011 | 100033 | 2194.3 | 11.3 | 7.9 | - | 201.9 | - |
| inverted | 262144 | 19.247 [19.118,19.518] (3) | 0.204 | 262182 | 17235.3 | 29.6 | 25.1 | - | 255.9 | - |

### early_presize

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.864 [1.860,1.914] (3) | 0.030 | 23 | 0.1 | 0.0 | 0.0 | - | 168.5 | - |
| alt | 100000 | 1.933 [1.909,1.946] (3) | 0.018 | 100030 | 12.4 | 8.2 | 7.9 | - | 202.8 | - |
| alt | 262144 | 2.081 [2.005,2.082] (3) | 0.044 | 262182 | 36.2 | 21.8 | 23.5 | - | 258.7 | - |
| alt | 1048576 | 2.833 [2.785,2.920] (3) | 0.068 | 1048658 | 199.5 | 86.4 | 257.4 | - | 530.4 | - |
| alt | 4194304 | 9.414 [9.169,9.428] (3) | 0.146 | 4194544 | 1900.0 | 348.1 | 3430.9 | - | 1626.7 | - |
| random | 262144 | 2.218 [2.213,2.232] (3) | 0.010 | 262228 | 43.5 | 40.2 | 39.9 | - | 396.6 | - |
| random | 1048576 | 3.892 [3.851,4.081] (3) | 0.123 | 1048818 | 316.0 | 160.8 | 580.6 | - | 1071.6 | - |
| inverted | 100000 | 4.126 [4.110,4.147] (3) | 0.019 | 100032 | 2186.7 | 11.2 | 8.8 | - | 202.4 | - |
| inverted | 262144 | 19.287 [18.877,19.478] (3) | 0.307 | 262184 | 17266.4 | 29.3 | 23.3 | - | 254.8 | - |

### early_bulk

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.855 [1.848,1.884] (3) | 0.019 | 22 | 0.1 | 0.0 | 0.0 | - | 167.8 | - |
| alt | 100000 | 1.923 [1.900,1.924] (3) | 0.013 | 100027 | 4.7 | 8.3 | 8.5 | - | 202.2 | - |
| alt | 262144 | 2.005 [1.991,2.059] (3) | 0.035 | 262184 | 12.5 | 21.7 | 23.9 | - | 259.3 | - |
| alt | 1048576 | 2.656 [2.607,2.708] (3) | 0.051 | 1048661 | 54.6 | 85.7 | 201.4 | - | 532.6 | - |
| alt | 4194304 | 7.597 [7.589,7.621] (3) | 0.017 | 4194555 | 214.0 | 346.9 | 3336.5 | - | 1633.6 | - |
| random | 262144 | 2.207 [2.202,2.228] (3) | 0.014 | 262225 | 13.4 | 40.6 | 37.6 | - | 394.7 | - |
| random | 1048576 | 3.530 [3.509,3.625] (3) | 0.062 | 1048819 | 55.3 | 159.2 | 500.4 | - | 1073.3 | - |
| inverted | 32768 | 1.891 [1.891,1.891] (1) | - | 32791 | 2.0 | 3.8 | 2.3 | - | 180.2 | - |
| inverted | 100000 | 1.925 [1.893,1.932] (3) | 0.021 | 100033 | 5.2 | 11.5 | 9.0 | - | 201.6 | - |
| inverted | 262144 | 2.051 [2.001,2.080] (3) | 0.040 | 262180 | 15.8 | 29.5 | 23.5 | - | 254.1 | - |
| inverted | 524288 | 2.262 [2.211,2.349] (3) | 0.069 | 524345 | 27.4 | 58.5 | 55.0 | - | 350.6 | - |
| inverted | 1048576 | 2.703 [2.674,2.706] (3) | 0.017 | 1048660 | 57.3 | 117.4 | 219.3 | - | 537.9 | - |
| inverted | 2097152 | 4.153 [4.140,4.221] (3) | 0.043 | 2097265 | 111.7 | 241.9 | 998.7 | - | 885.8 | - |
| inverted | 4194304 | 8.701 [8.692,9.008] (3) | 0.180 | 4194445 | 247.6 | 471.4 | 4273.6 | - | 1602.7 | - |

### early_nodefree

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.863 [1.855,1.881] (3) | 0.013 | 21 | 0.0 | 0.0 | 0.0 | 0.052 | 168.3 | - |
| alt | 100000 | 1.927 [1.917,1.939] (3) | 0.011 | 100032 | 13.5 | 8.4 | 0.0 | 0.037 | 203.6 | - |
| alt | 262144 | 2.034 [2.018,2.181] (3) | 0.089 | 262185 | 40.5 | 21.9 | 0.0 | 0.043 | 257.6 | - |
| alt | 1048576 | 2.622 [2.616,2.646] (3) | 0.016 | 1048664 | 209.6 | 85.6 | 0.0 | 0.064 | 536.4 | - |
| alt | 4194304 | 6.117 [6.072,6.470] (3) | 0.217 | 4194550 | 2063.1 | 348.0 | 0.0 | 0.142 | 1621.9 | - |
| random | 262144 | 2.193 [2.180,2.218] (3) | 0.019 | 262226 | 48.7 | 39.4 | 0.0 | 0.062 | 392.4 | - |
| random | 1048576 | 3.365 [3.340,3.375] (3) | 0.018 | 1048819 | 347.9 | 160.4 | 0.0 | 0.136 | 1073.8 | - |
| inverted | 100000 | 4.152 [4.080,4.172] (3) | 0.048 | 100032 | 2195.3 | 11.1 | 0.0 | 0.038 | 201.4 | - |
| inverted | 262144 | 19.393 [19.072,19.429] (3) | 0.196 | 262184 | 17410.4 | 29.0 | 0.0 | 0.042 | 253.3 | - |

### early_batchinit

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.882 [1.829,1.889] (3) | 0.032 | 23 | 0.0 | 0.0 | 0.0 | - | 168.1 | - |
| alt | 100000 | 1.939 [1.925,1.941] (3) | 0.008 | 100034 | 13.6 | 2.9 | 8.5 | - | 204.0 | - |
| alt | 262144 | 2.039 [2.038,2.049] (3) | 0.006 | 262187 | 40.4 | 7.0 | 23.3 | - | 257.8 | - |
| alt | 1048576 | 2.862 [2.813,3.042] (3) | 0.120 | 1048659 | 224.7 | 28.0 | 263.3 | - | 532.0 | - |
| alt | 4194304 | 9.286 [9.016,9.428] (3) | 0.209 | 4194554 | 2051.0 | 111.7 | 3390.8 | - | 1632.7 | - |
| random | 262144 | 2.196 [2.174,2.207] (3) | 0.017 | 262230 | 48.7 | 9.0 | 36.9 | - | 394.1 | - |
| random | 1048576 | 3.767 [3.729,3.964] (3) | 0.126 | 1048821 | 369.6 | 37.6 | 480.5 | - | 1072.6 | - |
| inverted | 100000 | 4.116 [4.102,4.133] (3) | 0.015 | 100033 | 2200.3 | 3.2 | 7.8 | - | 203.4 | - |
| inverted | 262144 | 19.321 [19.022,19.458] (3) | 0.223 | 262185 | 17343.8 | 8.0 | 23.7 | - | 257.4 | - |

### early_opt

| pattern | count | TTI median [min,max] (n) | sd | regions | walk1/retrieve ms | walk2 ms | free_nodes ms | late free ms | mfa ms | fallback |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 0 | 1.873 [1.847,1.890] (5) | 0.019 | 22 | 0.1 | 0.0 | 0.0 | 0.036 | 170.2 | - |
| contig | 1000 | 1.856 [1.847,1.872] (5) | 0.010 | 26 | 0.1 | 0.1 | 0.0 | 0.044 | 167.5 | - |
| contig | 10000 | 1.856 [1.838,1.863] (5) | 0.009 | 27 | 0.1 | 0.3 | 0.0 | 0.035 | 168.8 | - |
| contig | 100000 | 1.862 [1.859,1.911] (5) | 0.024 | 29 | 0.1 | 2.9 | 0.0 | 0.049 | 165.6 | - |
| contig | 1048576 | 1.887 [1.875,1.902] (5) | 0.010 | 54 | 0.1 | 25.1 | 0.0 | 0.057 | 157.3 | - |
| contig | 4194304 | 1.930 [1.905,1.945] (5) | 0.016 | 145 | 0.2 | 101.9 | 0.0 | 0.087 | 126.8 | - |
| contig | 8388608 | 1.997 [1.954,2.044] (5) | 0.036 | 251 | 0.4 | 204.1 | 0.0 | 0.147 | 83.0 | - |
| alt | 1000 | 1.866 [1.857,1.905] (5) | 0.020 | 1025 | 0.1 | 0.1 | 0.0 | 0.035 | 167.7 | - |
| alt | 4096 | 1.858 [1.851,1.891] (5) | 0.018 | 4121 | 0.5 | 0.2 | 0.0 | 0.041 | 167.9 | - |
| alt | 10000 | 1.868 [1.838,1.982] (5) | 0.058 | 10024 | 0.7 | 0.3 | 0.0 | 0.045 | 170.2 | - |
| alt | 32768 | 1.875 [1.867,1.878] (5) | 0.005 | 32794 | 1.7 | 1.1 | 0.0 | 0.038 | 180.1 | - |
| alt | 100000 | 1.930 [1.879,1.995] (5) | 0.044 | 100029 | 4.9 | 2.9 | 0.0 | 0.037 | 201.7 | - |
| alt | 262144 | 1.997 [1.978,2.019] (5) | 0.017 | 262184 | 13.1 | 7.3 | 0.0 | 0.042 | 258.4 | - |
| alt | 1048576 | 2.415 [2.394,2.420] (5) | 0.012 | 1048659 | 54.6 | 29.0 | 0.0 | 0.064 | 533.5 | - |
| alt | 2097152 | 2.962 [2.934,3.003] (5) | 0.026 | 2097291 | 112.1 | 57.1 | 0.0 | 0.094 | 900.4 | - |
| alt | 4194304 | 4.034 [4.018,4.157] (5) | 0.057 | 4194546 | 218.0 | 115.5 | 0.0 | 0.149 | 1625.5 | - |
| random | 1000 | 1.845 [1.833,1.998] (5) | 0.069 | 1024 | 0.1 | 0.1 | 0.0 | 0.036 | 167.0 | - |
| random | 4096 | 1.863 [1.855,1.881] (5) | 0.010 | 4122 | 0.5 | 0.2 | 0.0 | 0.034 | 170.7 | - |
| random | 10000 | 1.879 [1.858,2.125] (5) | 0.114 | 10028 | 0.7 | 0.4 | 0.0 | 0.035 | 175.6 | - |
| random | 32768 | 1.898 [1.869,1.922] (5) | 0.021 | 32800 | 1.8 | 1.2 | 0.0 | 0.038 | 196.4 | - |
| random | 100000 | 1.971 [1.935,1.995] (5) | 0.023 | 100047 | 5.1 | 3.5 | 0.0 | 0.045 | 254.6 | - |
| random | 262144 | 2.123 [2.114,2.147] (5) | 0.013 | 262225 | 12.9 | 9.3 | 0.0 | 0.062 | 396.8 | - |
| random | 1048576 | 2.964 [2.931,2.982] (5) | 0.019 | 1048816 | 56.5 | 36.8 | 0.0 | 0.145 | 1070.2 | - |
| random | 1572864 | 3.505 [3.486,3.530] (3) | 0.022 | 1573198 | 85.6 | 57.1 | 0.0 | 0.242 | 1528.8 | - |
| mixed | 1000 | 1.869 [1.855,1.876] (5) | 0.008 | 1049 | 0.2 | 0.1 | 0.0 | 0.049 | 166.3 | - |
| mixed | 4096 | 1.849 [1.832,1.937] (5) | 0.042 | 4203 | 0.8 | 0.3 | 0.0 | 0.085 | 162.2 | - |
| mixed | 10000 | 1.855 [1.848,1.876] (5) | 0.012 | 10201 | 1.4 | 0.6 | 0.0 | 0.132 | 153.3 | - |
| mixed | 32768 | 1.807 [1.801,1.862] (5) | 0.025 | 33152 | 4.0 | 1.8 | 0.0 | 0.463 | 125.2 | - |
| mixed | 40960 | 1.830 [1.818,1.855] (5) | 0.014 | 41367 | 4.8 | 2.2 | 0.0 | 0.473 | 114.8 | - |
| inverted | 1000 | 1.866 [1.845,2.085] (5) | 0.100 | 1027 | 0.1 | 0.1 | 0.0 | 0.046 | 167.6 | - |
| inverted | 4096 | 1.869 [1.829,1.878] (5) | 0.020 | 4122 | 0.5 | 0.2 | 0.0 | 0.036 | 167.6 | - |
| inverted | 10000 | 1.885 [1.857,1.895] (5) | 0.016 | 10027 | 0.7 | 0.4 | 0.0 | 0.041 | 173.0 | - |
| inverted | 32768 | 1.886 [1.878,1.904] (5) | 0.011 | 32794 | 1.9 | 1.1 | 0.0 | 0.037 | 179.5 | - |
| inverted | 100000 | 1.930 [1.895,1.961] (5) | 0.025 | 100033 | 5.3 | 3.2 | 0.0 | 0.050 | 202.1 | - |
| inverted | 262144 | 2.005 [1.972,2.042] (5) | 0.025 | 262184 | 13.7 | 8.4 | 0.0 | 0.043 | 257.0 | - |
| inverted | 524288 | 2.135 [2.125,2.145] (5) | 0.010 | 524340 | 27.1 | 16.8 | 0.0 | 0.050 | 346.9 | - |
| inverted | 1048576 | 2.405 [2.385,2.415] (5) | 0.012 | 1048661 | 56.5 | 31.8 | 0.0 | 0.070 | 526.6 | - |
| inverted | 2097152 | 2.969 [2.932,2.972] (5) | 0.019 | 2097268 | 119.2 | 64.8 | 0.0 | 0.093 | 888.4 | - |
| inverted | 4194304 | 4.082 [4.023,4.103] (3) | 0.042 | 4194458 | 243.4 | 129.6 | 0.0 | 0.131 | 1599.8 | - |
| inverted | 8388608 | no ok run (1 rows) | - | - | - | - | - | - | - | - |

## walk1 sub-timers (ms, medians)

streams = number of sorted streams merged by memblock_reserve_bulk() (set of values seen in the ok runs); bulk cost is O(streams x (n + R)) for n new ranges and R existing regions.  reserved array MiB = reserved.max x 24 B (sizeof(struct memblock_region), x86_64 NUMA) from the logged max; temp MiB = page-aligned (ranges + nodes) x 24 B, the bulk/opt temporary array, allocated while noprsrv-only (KHO scratch) is on and freed after walk 1.  Both are derived from logged counts, not measured allocations.

| mode | pattern | count | walk1 | count | presize | alloc | collect | sort | bulk | node | ranges | streams | reserved.max | reserved array MiB | temp MiB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| early_noinit1 | none | 0 | 0.03 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.00 | 35 | 0 | 128 | 0.0 | - |
| early_noinit1 | alt | 100000 | 9.79 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.35 | 100022 | 0 | 131072 | 3.0 | - |
| early_noinit1 | alt | 262144 | 30.07 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 2.15 | 262167 | 0 | 524288 | 12.0 | - |
| early_noinit1 | alt | 1048576 | 172.10 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 48.61 | 1048607 | 0 | 2097152 | 48.0 | - |
| early_noinit1 | alt | 4194304 | 1795.32 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 1244.86 | 4194328 | 0 | 8388608 | 192.0 | - |
| early_noinit1 | random | 262144 | 38.76 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 10.23 | 262168 | 0 | 524288 | 12.0 | - |
| early_noinit1 | random | 1048576 | 304.69 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 179.51 | 1048602 | 0 | 2097152 | 48.0 | - |
| early_noinit1 | inverted | 100000 | 2204.64 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.94 | 100026 | 0 | 131072 | 3.0 | - |
| early_noinit1 | inverted | 262144 | 17179.62 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 3.76 | 262164 | 0 | 524288 | 12.0 | - |
| early_noinit1 | inverted | 524288 | 67890.35 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 12.49 | 524309 | 0 | 1048576 | 24.0 | - |
| early_coalesce | none | 0 | 0.03 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.00 | 17 | 0 | 128 | 0.0 | - |
| early_coalesce | contig | 1048576 | 0.06 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.01 | 15 | 0 | 128 | 0.0 | - |
| early_coalesce | contig | 8388608 | 0.22 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.05 | 20 | 0 | 256 | 0.0 | - |
| early_coalesce | alt | 100000 | 12.92 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.35 | 100016 | 0 | 131072 | 3.0 | - |
| early_coalesce | alt | 262144 | 39.48 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 2.18 | 262164 | 0 | 524288 | 12.0 | - |
| early_coalesce | alt | 1048576 | 217.78 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 54.89 | 1048591 | 0 | 2097152 | 48.0 | - |
| early_coalesce | alt | 4194304 | 1987.59 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 1257.13 | 4194318 | 0 | 8388608 | 192.0 | - |
| early_coalesce | random | 262144 | 47.03 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 9.61 | 262159 | 0 | 524288 | 12.0 | - |
| early_coalesce | random | 1048576 | 334.05 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 170.78 | 1048591 | 0 | 2097152 | 48.0 | - |
| early_coalesce | inverted | 100000 | 2194.26 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.88 | 100017 | 0 | 131072 | 3.0 | - |
| early_coalesce | inverted | 262144 | 17235.33 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 3.78 | 262157 | 0 | 524288 | 12.0 | - |
| early_presize | none | 0 | 0.05 | 0.03 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.00 | 36 | 0 | 236 | 0.0 | - |
| early_presize | alt | 100000 | 12.43 | 1.77 | 1.09 | 0.000 | 0.00 | 0.00 | 0.00 | 0.34 | 100022 | 0 | 200216 | 4.6 | - |
| early_presize | alt | 262144 | 36.18 | 5.76 | 2.56 | 0.000 | 0.00 | 0.00 | 0.00 | 2.45 | 262171 | 0 | 524532 | 12.0 | - |
| early_presize | alt | 1048576 | 199.53 | 21.34 | 9.95 | 0.000 | 0.00 | 0.00 | 0.00 | 57.83 | 1048603 | 0 | 2097490 | 48.0 | - |
| early_presize | alt | 4194304 | 1899.99 | 88.32 | 39.60 | 0.000 | 0.00 | 0.00 | 0.00 | 1309.35 | 4194330 | 0 | 8389326 | 192.0 | - |
| early_presize | random | 262144 | 43.46 | 5.98 | 2.77 | 0.000 | 0.00 | 0.00 | 0.00 | 9.58 | 262170 | 0 | 524636 | 12.0 | - |
| early_presize | random | 1048576 | 316.04 | 20.51 | 10.04 | 0.000 | 0.00 | 0.00 | 0.00 | 178.21 | 1048599 | 0 | 2097870 | 48.0 | - |
| early_presize | inverted | 100000 | 2186.72 | 2.22 | 1.10 | 0.000 | 0.00 | 0.00 | 0.00 | 0.93 | 100025 | 0 | 200232 | 4.6 | - |
| early_presize | inverted | 262144 | 17266.36 | 5.76 | 2.61 | 0.000 | 0.00 | 0.00 | 0.00 | 3.84 | 262170 | 0 | 524528 | 12.0 | - |
| early_bulk | none | 0 | 0.05 | 0.03 | 0.00 | 0.000 | 0.01 | 0.00 | 0.00 | 0.00 | 14 | 6,9 | 178 | 0.0 | 0.0 |
| early_bulk | alt | 100000 | 4.74 | 1.08 | 1.10 | 0.001 | 1.58 | 0.01 | 0.95 | 0.00 | 100014 | 4,8,9 | 200210 | 4.6 | 2.3 |
| early_bulk | alt | 262144 | 12.47 | 2.83 | 2.70 | 0.001 | 4.20 | 0.01 | 2.66 | 0.00 | 262161 | 6,8,9 | 524516 | 12.0 | 6.0 |
| early_bulk | alt | 1048576 | 54.60 | 12.31 | 9.98 | 0.001 | 18.63 | 0.02 | 12.72 | 0.00 | 1048594 | 6,9 | 2097484 | 48.0 | 24.0 |
| early_bulk | alt | 4194304 | 214.02 | 44.35 | 38.76 | 0.001 | 62.70 | 0.05 | 53.20 | 0.01 | 4194318 | 6,7,8 | 8389308 | 192.0 | 96.0 |
| early_bulk | random | 262144 | 13.36 | 3.15 | 2.70 | 0.001 | 4.74 | 0.02 | 2.70 | 0.00 | 262158 | 6,7,8 | 524604 | 12.0 | 6.0 |
| early_bulk | random | 1048576 | 55.26 | 12.53 | 9.71 | 0.001 | 18.50 | 0.05 | 11.97 | 0.01 | 1048590 | 6,7,8 | 2097852 | 48.0 | 24.0 |
| early_bulk | inverted | 32768 | 2.00 | 0.42 | 0.41 | 0.001 | 0.82 | 0.01 | 0.34 | 0.00 | 32783 | 8 | 65730 | 1.5 | 0.8 |
| early_bulk | inverted | 100000 | 5.23 | 1.19 | 1.10 | 0.001 | 1.76 | 0.01 | 1.20 | 0.00 | 100018 | 6,9 | 200220 | 4.6 | 2.3 |
| early_bulk | inverted | 262144 | 15.83 | 3.08 | 2.67 | 0.001 | 4.72 | 0.01 | 3.24 | 0.00 | 262163 | 8,9 | 524518 | 12.0 | 6.0 |
| early_bulk | inverted | 524288 | 27.37 | 6.17 | 5.11 | 0.001 | 9.35 | 0.01 | 6.72 | 0.00 | 524305 | 8,9 | 1048836 | 24.0 | 12.0 |
| early_bulk | inverted | 1048576 | 57.26 | 12.28 | 9.92 | 0.001 | 18.55 | 0.02 | 15.20 | 0.00 | 1048590 | 6,8,9 | 2097474 | 48.0 | 24.0 |
| early_bulk | inverted | 2097152 | 111.73 | 22.14 | 19.94 | 0.001 | 31.32 | 0.03 | 33.13 | 0.00 | 2097167 | 7,9 | 4194756 | 96.0 | 48.0 |
| early_bulk | inverted | 4194304 | 247.57 | 49.24 | 40.00 | 0.001 | 74.34 | 0.05 | 67.54 | 0.01 | 4194320 | 7,8,9 | 8389320 | 192.0 | 96.0 |
| early_nodefree | none | 0 | 0.03 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.00 | 20 | 0 | 128 | 0.0 | - |
| early_nodefree | alt | 100000 | 13.46 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.35 | 100026 | 0 | 131072 | 3.0 | - |
| early_nodefree | alt | 262144 | 40.50 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 2.01 | 262169 | 0 | 524288 | 12.0 | - |
| early_nodefree | alt | 1048576 | 209.63 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 42.07 | 1048604 | 0 | 2097152 | 48.0 | - |
| early_nodefree | alt | 4194304 | 2063.13 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 1314.03 | 4194330 | 0 | 8388608 | 192.0 | - |
| early_nodefree | random | 262144 | 48.71 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 10.16 | 262169 | 0 | 524288 | 12.0 | - |
| early_nodefree | random | 1048576 | 347.86 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 180.42 | 1048602 | 0 | 2097152 | 48.0 | - |
| early_nodefree | inverted | 100000 | 2195.29 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.96 | 100027 | 0 | 131072 | 3.0 | - |
| early_nodefree | inverted | 262144 | 17410.36 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 3.94 | 262172 | 0 | 524288 | 12.0 | - |
| early_batchinit | none | 0 | 0.03 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.00 | 22 | 0 | 128 | 0.0 | - |
| early_batchinit | alt | 100000 | 13.64 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 0.36 | 100033 | 0 | 131072 | 3.0 | - |
| early_batchinit | alt | 262144 | 40.37 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 1.95 | 262172 | 0 | 524288 | 12.0 | - |
| early_batchinit | alt | 1048576 | 224.71 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 55.62 | 1048605 | 0 | 2097152 | 48.0 | - |
| early_batchinit | alt | 4194304 | 2051.02 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 1296.07 | 4194334 | 0 | 8388608 | 192.0 | - |
| early_batchinit | random | 262144 | 48.67 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 9.58 | 262174 | 0 | 524288 | 12.0 | - |
| early_batchinit | random | 1048576 | 369.60 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 200.48 | 1048608 | 0 | 2097152 | 48.0 | - |
| early_batchinit | inverted | 100000 | 2200.26 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 1.03 | 100025 | 0 | 131072 | 3.0 | - |
| early_batchinit | inverted | 262144 | 17343.79 | 0.00 | 0.00 | 0.000 | 0.00 | 0.00 | 0.00 | 3.84 | 262168 | 0 | 524288 | 12.0 | - |
| early_opt | none | 0 | 0.05 | 0.03 | 0.00 | 0.000 | 0.02 | 0.01 | 0.00 | 0.00 | 15 | 7,8,10 | 196 | 0.0 | 0.0 |
| early_opt | contig | 1000 | 0.05 | 0.03 | 0.00 | 0.000 | 0.02 | 0.01 | 0.00 | 0.00 | 19 | 5,8,9,10 | 200 | 0.0 | 0.0 |
| early_opt | contig | 10000 | 0.06 | 0.03 | 0.00 | 0.000 | 0.02 | 0.01 | 0.00 | 0.00 | 17 | 6,7,8 | 196 | 0.0 | 0.0 |
| early_opt | contig | 100000 | 0.05 | 0.03 | 0.00 | 0.000 | 0.02 | 0.01 | 0.00 | 0.00 | 18 | 6,7,9 | 202 | 0.0 | 0.0 |
| early_opt | contig | 1048576 | 0.09 | 0.05 | 0.00 | 0.000 | 0.03 | 0.01 | 0.00 | 0.00 | 16 | 6,7,8,9 | 258 | 0.0 | 0.0 |
| early_opt | contig | 4194304 | 0.19 | 0.10 | 0.00 | 0.000 | 0.06 | 0.03 | 0.00 | 0.00 | 18 | 7,8,9 | 452 | 0.0 | 0.0 |
| early_opt | contig | 8388608 | 0.37 | 0.18 | 0.00 | 0.000 | 0.13 | 0.05 | 0.00 | 0.00 | 18 | 6,8,9,10 | 714 | 0.0 | 0.0 |
| early_opt | alt | 1000 | 0.09 | 0.04 | 0.01 | 0.000 | 0.03 | 0.01 | 0.01 | 0.00 | 1016 | 7,8,9 | 2204 | 0.1 | 0.0 |
| early_opt | alt | 4096 | 0.51 | 0.07 | 0.02 | 0.000 | 0.37 | 0.01 | 0.04 | 0.29 | 4111 | 6,7,8,9 | 8384 | 0.2 | 0.1 |
| early_opt | alt | 10000 | 0.69 | 0.14 | 0.30 | 0.001 | 0.17 | 0.01 | 0.08 | 0.00 | 10019 | 5,7,8,9 | 20206 | 0.5 | 0.2 |
| early_opt | alt | 32768 | 1.67 | 0.37 | 0.38 | 0.001 | 0.64 | 0.01 | 0.26 | 0.00 | 32786 | 6,7,8 | 65744 | 1.5 | 0.8 |
| early_opt | alt | 100000 | 4.94 | 1.19 | 1.11 | 0.000 | 1.70 | 0.01 | 0.96 | 0.00 | 100015 | 7 | 200204 | 4.6 | 2.3 |
| early_opt | alt | 262144 | 13.08 | 3.08 | 2.66 | 0.001 | 4.63 | 0.01 | 2.71 | 0.00 | 262161 | 5,6,8,9 | 524522 | 12.0 | 6.0 |
| early_opt | alt | 1048576 | 54.57 | 12.36 | 10.10 | 0.001 | 18.47 | 0.02 | 12.83 | 0.00 | 1048594 | 4,7,9,10,11 | 2097484 | 48.0 | 24.0 |
| early_opt | alt | 2097152 | 112.09 | 23.62 | 20.28 | 0.001 | 32.19 | 0.03 | 27.20 | 0.01 | 2097167 | 6,7,8,9 | 4194756 | 96.0 | 48.0 |
| early_opt | alt | 4194304 | 217.97 | 44.38 | 40.09 | 0.001 | 63.30 | 0.05 | 54.73 | 0.01 | 4194320 | 8 | 8389316 | 192.0 | 96.0 |
| early_opt | random | 1000 | 0.10 | 0.04 | 0.01 | 0.000 | 0.03 | 0.01 | 0.01 | 0.00 | 1016 | 7,8,9 | 2200 | 0.1 | 0.0 |
| early_opt | random | 4096 | 0.52 | 0.08 | 0.02 | 0.000 | 0.37 | 0.01 | 0.04 | 0.29 | 4112 | 5,8 | 8388 | 0.2 | 0.1 |
| early_opt | random | 10000 | 0.70 | 0.14 | 0.30 | 0.001 | 0.17 | 0.01 | 0.08 | 0.00 | 10017 | 4,6,7,8 | 20200 | 0.5 | 0.2 |
| early_opt | random | 32768 | 1.82 | 0.41 | 0.38 | 0.001 | 0.74 | 0.01 | 0.28 | 0.00 | 32787 | 7,8,9 | 65752 | 1.5 | 0.8 |
| early_opt | random | 100000 | 5.06 | 1.23 | 1.11 | 0.001 | 1.78 | 0.01 | 0.97 | 0.00 | 100015 | 3,6,8 | 200240 | 4.6 | 2.3 |
| early_opt | random | 262144 | 12.85 | 3.07 | 2.73 | 0.001 | 4.30 | 0.02 | 2.68 | 0.00 | 262159 | 7,8,9 | 524614 | 12.0 | 6.0 |
| early_opt | random | 1048576 | 56.51 | 12.53 | 9.66 | 0.001 | 18.63 | 0.05 | 12.52 | 0.01 | 1048592 | 8,9 | 2097858 | 48.0 | 24.0 |
| early_opt | random | 1572864 | 85.64 | 18.74 | 15.02 | 0.001 | 27.73 | 0.08 | 19.61 | 0.01 | 1572883 | 8,9 | 3146700 | 72.0 | 36.0 |
| early_opt | mixed | 1000 | 0.17 | 0.06 | 0.01 | 0.000 | 0.05 | 0.01 | 0.04 | 0.00 | 1016 | 11 | 2252 | 0.1 | 0.0 |
| early_opt | mixed | 4096 | 0.77 | 0.14 | 0.02 | 0.000 | 0.43 | 0.02 | 0.17 | 0.29 | 4110 | 11 | 8564 | 0.2 | 0.1 |
| early_opt | mixed | 10000 | 1.37 | 0.29 | 0.31 | 0.001 | 0.30 | 0.04 | 0.42 | 0.01 | 10014 | 11 | 20614 | 0.5 | 0.2 |
| early_opt | mixed | 32768 | 4.00 | 0.87 | 0.40 | 0.001 | 1.17 | 0.14 | 1.36 | 0.02 | 32780 | 11 | 67060 | 1.5 | 0.8 |
| early_opt | mixed | 40960 | 4.84 | 1.07 | 0.42 | 0.001 | 1.40 | 0.17 | 1.67 | 0.02 | 40969 | 11 | 83760 | 1.9 | 1.0 |
| early_opt | inverted | 1000 | 0.09 | 0.04 | 0.01 | 0.000 | 0.03 | 0.01 | 0.01 | 0.00 | 1016 | 7,8,9 | 2200 | 0.1 | 0.0 |
| early_opt | inverted | 4096 | 0.52 | 0.07 | 0.02 | 0.000 | 0.38 | 0.01 | 0.05 | 0.30 | 4112 | 7,8,10 | 8388 | 0.2 | 0.1 |
| early_opt | inverted | 10000 | 0.74 | 0.14 | 0.31 | 0.001 | 0.17 | 0.01 | 0.10 | 0.00 | 10018 | 8,9,10 | 20208 | 0.5 | 0.2 |
| early_opt | inverted | 32768 | 1.89 | 0.41 | 0.39 | 0.001 | 0.76 | 0.01 | 0.32 | 0.00 | 32786 | 6,9,10 | 65742 | 1.5 | 0.8 |
| early_opt | inverted | 100000 | 5.30 | 1.20 | 1.13 | 0.001 | 1.75 | 0.01 | 1.18 | 0.00 | 100018 | 8,9 | 200216 | 4.6 | 2.3 |
| early_opt | inverted | 262144 | 13.70 | 3.10 | 2.69 | 0.001 | 4.63 | 0.01 | 3.19 | 0.00 | 262159 | 5,8,9,10 | 524512 | 12.0 | 6.0 |
| early_opt | inverted | 524288 | 27.10 | 6.20 | 5.05 | 0.001 | 9.26 | 0.01 | 6.68 | 0.00 | 524303 | 6,7,8,9 | 1048840 | 24.0 | 12.0 |
| early_opt | inverted | 1048576 | 56.47 | 12.29 | 9.87 | 0.001 | 18.45 | 0.02 | 14.84 | 0.00 | 1048593 | 5,7,8,9 | 2097480 | 48.0 | 24.0 |
| early_opt | inverted | 2097152 | 119.25 | 24.57 | 20.05 | 0.001 | 36.93 | 0.03 | 32.38 | 0.01 | 2097167 | 6,7,8,9 | 4194750 | 96.0 | 48.0 |
| early_opt | inverted | 4194304 | 243.43 | 49.21 | 39.73 | 0.001 | 73.89 | 0.05 | 67.87 | 0.01 | 4194319 | 7,8 | 8389318 | 192.0 | 96.0 |

## mode minus early (median TTI, same pattern and count)

| mode | pattern | count | mode TTI | early TTI | delta s | p (MWU) |
|---|---|---|---|---|---|---|
| early_noinit1 | none | 0 | 1.859 [1.858,1.876] (3) | 1.854 [1.843,1.886] (3) | +0.005 | 0.700 |
| early_noinit1 | alt | 1048576 | 2.782 [2.663,2.854] (3) | 2.931 [2.863,2.961] (5) | -0.149 | 0.036 |
| early_noinit1 | alt | 4194304 | 9.144 [8.990,9.208] (3) | 9.420 [9.279,9.599] (5) | -0.276 | 0.036 |
| early_noinit1 | random | 1048576 | 3.877 [3.735,4.039] (3) | 3.886 [3.814,4.015] (5) | -0.009 | 0.786 |
| early_noinit1 | inverted | 100000 | 4.107 [4.075,4.121] (3) | 4.111 [4.085,4.164] (5) | -0.003 | 0.393 |
| early_noinit1 | inverted | 262144 | 19.200 [19.156,19.248] (3) | 19.292 [19.040,19.532] (3) | -0.091 | 0.700 |
| early_noinit1 | inverted | 524288 | 70.074 [70.074,70.074] (1) | 71.100 [71.100,71.100] (1) | -1.026 | - |
| early_coalesce | none | 0 | 1.866 [1.863,1.883] (3) | 1.854 [1.843,1.886] (3) | +0.011 | 0.700 |
| early_coalesce | contig | 8388608 | 2.400 [2.399,2.424] (3) | 2.969 [2.962,3.026] (5) | -0.569 | 0.036 |
| early_coalesce | alt | 1048576 | 2.868 [2.822,2.888] (3) | 2.931 [2.863,2.961] (5) | -0.062 | 0.393 |
| early_coalesce | alt | 4194304 | 9.390 [9.288,9.418] (3) | 9.420 [9.279,9.599] (5) | -0.030 | 0.393 |
| early_coalesce | random | 1048576 | 3.856 [3.773,4.015] (3) | 3.886 [3.814,4.015] (5) | -0.030 | 0.571 |
| early_coalesce | inverted | 100000 | 4.097 [4.089,4.111] (3) | 4.111 [4.085,4.164] (5) | -0.014 | 0.571 |
| early_coalesce | inverted | 262144 | 19.247 [19.118,19.518] (3) | 19.292 [19.040,19.532] (3) | -0.045 | 1.000 |
| early_presize | none | 0 | 1.864 [1.860,1.914] (3) | 1.854 [1.843,1.886] (3) | +0.010 | 0.400 |
| early_presize | alt | 1048576 | 2.833 [2.785,2.920] (3) | 2.931 [2.863,2.961] (5) | -0.097 | 0.143 |
| early_presize | alt | 4194304 | 9.414 [9.169,9.428] (3) | 9.420 [9.279,9.599] (5) | -0.006 | 0.393 |
| early_presize | random | 1048576 | 3.892 [3.851,4.081] (3) | 3.886 [3.814,4.015] (5) | +0.006 | 0.786 |
| early_presize | inverted | 100000 | 4.126 [4.110,4.147] (3) | 4.111 [4.085,4.164] (5) | +0.015 | 1.000 |
| early_presize | inverted | 262144 | 19.287 [18.877,19.478] (3) | 19.292 [19.040,19.532] (3) | -0.005 | 0.700 |
| early_bulk | none | 0 | 1.855 [1.848,1.884] (3) | 1.854 [1.843,1.886] (3) | +0.001 | 1.000 |
| early_bulk | alt | 1048576 | 2.656 [2.607,2.708] (3) | 2.931 [2.863,2.961] (5) | -0.275 | 0.036 |
| early_bulk | alt | 4194304 | 7.597 [7.589,7.621] (3) | 9.420 [9.279,9.599] (5) | -1.824 | 0.036 |
| early_bulk | random | 1048576 | 3.530 [3.509,3.625] (3) | 3.886 [3.814,4.015] (5) | -0.356 | 0.036 |
| early_bulk | inverted | 32768 | 1.891 [1.891,1.891] (1) | 2.108 [2.108,2.108] (1) | -0.217 | - |
| early_bulk | inverted | 100000 | 1.925 [1.893,1.932] (3) | 4.111 [4.085,4.164] (5) | -2.185 | 0.036 |
| early_bulk | inverted | 262144 | 2.051 [2.001,2.080] (3) | 19.292 [19.040,19.532] (3) | -17.240 | 0.100 |
| early_bulk | inverted | 524288 | 2.262 [2.211,2.349] (3) | 71.100 [71.100,71.100] (1) | -68.838 | - |
| early_nodefree | none | 0 | 1.863 [1.855,1.881] (3) | 1.854 [1.843,1.886] (3) | +0.008 | 0.700 |
| early_nodefree | alt | 1048576 | 2.622 [2.616,2.646] (3) | 2.931 [2.863,2.961] (5) | -0.308 | 0.036 |
| early_nodefree | alt | 4194304 | 6.117 [6.072,6.470] (3) | 9.420 [9.279,9.599] (5) | -3.303 | 0.036 |
| early_nodefree | random | 1048576 | 3.365 [3.340,3.375] (3) | 3.886 [3.814,4.015] (5) | -0.521 | 0.036 |
| early_nodefree | inverted | 100000 | 4.152 [4.080,4.172] (3) | 4.111 [4.085,4.164] (5) | +0.041 | 0.786 |
| early_nodefree | inverted | 262144 | 19.393 [19.072,19.429] (3) | 19.292 [19.040,19.532] (3) | +0.101 | 1.000 |
| early_batchinit | none | 0 | 1.882 [1.829,1.889] (3) | 1.854 [1.843,1.886] (3) | +0.028 | 1.000 |
| early_batchinit | alt | 1048576 | 2.862 [2.813,3.042] (3) | 2.931 [2.863,2.961] (5) | -0.069 | 0.571 |
| early_batchinit | alt | 4194304 | 9.286 [9.016,9.428] (3) | 9.420 [9.279,9.599] (5) | -0.135 | 0.393 |
| early_batchinit | random | 1048576 | 3.767 [3.729,3.964] (3) | 3.886 [3.814,4.015] (5) | -0.119 | 0.393 |
| early_batchinit | inverted | 100000 | 4.116 [4.102,4.133] (3) | 4.111 [4.085,4.164] (5) | +0.005 | 1.000 |
| early_batchinit | inverted | 262144 | 19.321 [19.022,19.458] (3) | 19.292 [19.040,19.532] (3) | +0.029 | 1.000 |
| early_opt | none | 0 | 1.873 [1.847,1.890] (5) | 1.854 [1.843,1.886] (3) | +0.019 | 0.786 |
| early_opt | contig | 8388608 | 1.997 [1.954,2.044] (5) | 2.969 [2.962,3.026] (5) | -0.972 | 0.008 |
| early_opt | alt | 1048576 | 2.415 [2.394,2.420] (5) | 2.931 [2.863,2.961] (5) | -0.515 | 0.008 |
| early_opt | alt | 4194304 | 4.034 [4.018,4.157] (5) | 9.420 [9.279,9.599] (5) | -5.386 | 0.008 |
| early_opt | random | 1048576 | 2.964 [2.931,2.982] (5) | 3.886 [3.814,4.015] (5) | -0.922 | 0.008 |
| early_opt | mixed | 32768 | 1.807 [1.801,1.862] (5) | 2.127 [2.108,2.156] (5) | -0.320 | 0.008 |
| early_opt | inverted | 10000 | 1.885 [1.857,1.895] (5) | 1.907 [1.907,1.907] (1) | -0.022 | - |
| early_opt | inverted | 32768 | 1.886 [1.878,1.904] (5) | 2.108 [2.108,2.108] (1) | -0.222 | - |
| early_opt | inverted | 100000 | 1.930 [1.895,1.961] (5) | 4.111 [4.085,4.164] (5) | -2.180 | 0.008 |
| early_opt | inverted | 262144 | 2.005 [1.972,2.042] (5) | 19.292 [19.040,19.532] (3) | -17.287 | 0.036 |
| early_opt | inverted | 524288 | 2.135 [2.125,2.145] (5) | 71.100 [71.100,71.100] (1) | -68.965 | - |

## mode minus baseline (median TTI, same pattern and count)

| mode | pattern | count | mode TTI | baseline TTI | delta s | p (MWU) |
|---|---|---|---|---|---|---|
| early | none | 0 | 1.854 [1.843,1.886] (3) | 1.848 [1.828,1.884] (3) | +0.007 | 0.700 |
| early | contig | 8388608 | 2.969 [2.962,3.026] (5) | 3.909 [3.902,3.996] (5) | -0.940 | 0.008 |
| early | alt | 1048576 | 2.931 [2.863,2.961] (5) | 2.600 [2.562,2.648] (5) | +0.330 | 0.008 |
| early | alt | 4194304 | 9.420 [9.279,9.599] (5) | 4.850 [4.837,5.011] (5) | +4.570 | 0.008 |
| early | random | 1048576 | 3.886 [3.814,4.015] (5) | 3.165 [3.156,3.226] (5) | +0.720 | 0.008 |
| early | mixed | 32768 | 2.127 [2.108,2.156] (5) | 2.057 [2.050,2.076] (5) | +0.070 | 0.008 |
| early | inverted | 10000 | 1.907 [1.907,1.907] (1) | 1.857 [1.857,1.857] (1) | +0.049 | - |
| early | inverted | 32768 | 2.108 [2.108,2.108] (1) | 2.082 [2.082,2.082] (1) | +0.026 | - |
| early | inverted | 100000 | 4.111 [4.085,4.164] (5) | 4.106 [4.067,4.129] (5) | +0.005 | 0.421 |
| early | inverted | 262144 | 19.292 [19.040,19.532] (3) | 19.204 [19.080,19.386] (3) | +0.088 | 1.000 |
| early | inverted | 524288 | 71.100 [71.100,71.100] (1) | 71.705 [71.705,71.705] (1) | -0.605 | - |
| early_noinit1 | none | 0 | 1.859 [1.858,1.876] (3) | 1.848 [1.828,1.884] (3) | +0.012 | 0.700 |
| early_noinit1 | alt | 1048576 | 2.782 [2.663,2.854] (3) | 2.600 [2.562,2.648] (5) | +0.182 | 0.036 |
| early_noinit1 | alt | 4194304 | 9.144 [8.990,9.208] (3) | 4.850 [4.837,5.011] (5) | +4.294 | 0.036 |
| early_noinit1 | random | 1048576 | 3.877 [3.735,4.039] (3) | 3.165 [3.156,3.226] (5) | +0.711 | 0.036 |
| early_noinit1 | inverted | 100000 | 4.107 [4.075,4.121] (3) | 4.106 [4.067,4.129] (5) | +0.001 | 1.000 |
| early_noinit1 | inverted | 262144 | 19.200 [19.156,19.248] (3) | 19.204 [19.080,19.386] (3) | -0.004 | 1.000 |
| early_noinit1 | inverted | 524288 | 70.074 [70.074,70.074] (1) | 71.705 [71.705,71.705] (1) | -1.631 | - |
| early_coalesce | none | 0 | 1.866 [1.863,1.883] (3) | 1.848 [1.828,1.884] (3) | +0.018 | 0.700 |
| early_coalesce | contig | 8388608 | 2.400 [2.399,2.424] (3) | 3.909 [3.902,3.996] (5) | -1.509 | 0.036 |
| early_coalesce | alt | 1048576 | 2.868 [2.822,2.888] (3) | 2.600 [2.562,2.648] (5) | +0.268 | 0.036 |
| early_coalesce | alt | 4194304 | 9.390 [9.288,9.418] (3) | 4.850 [4.837,5.011] (5) | +4.540 | 0.036 |
| early_coalesce | random | 1048576 | 3.856 [3.773,4.015] (3) | 3.165 [3.156,3.226] (5) | +0.691 | 0.036 |
| early_coalesce | inverted | 100000 | 4.097 [4.089,4.111] (3) | 4.106 [4.067,4.129] (5) | -0.009 | 0.786 |
| early_coalesce | inverted | 262144 | 19.247 [19.118,19.518] (3) | 19.204 [19.080,19.386] (3) | +0.043 | 0.700 |
| early_presize | none | 0 | 1.864 [1.860,1.914] (3) | 1.848 [1.828,1.884] (3) | +0.016 | 0.400 |
| early_presize | alt | 1048576 | 2.833 [2.785,2.920] (3) | 2.600 [2.562,2.648] (5) | +0.233 | 0.036 |
| early_presize | alt | 4194304 | 9.414 [9.169,9.428] (3) | 4.850 [4.837,5.011] (5) | +4.564 | 0.036 |
| early_presize | random | 1048576 | 3.892 [3.851,4.081] (3) | 3.165 [3.156,3.226] (5) | +0.726 | 0.036 |
| early_presize | inverted | 100000 | 4.126 [4.110,4.147] (3) | 4.106 [4.067,4.129] (5) | +0.020 | 0.250 |
| early_presize | inverted | 262144 | 19.287 [18.877,19.478] (3) | 19.204 [19.080,19.386] (3) | +0.083 | 1.000 |
| early_bulk | none | 0 | 1.855 [1.848,1.884] (3) | 1.848 [1.828,1.884] (3) | +0.007 | 0.400 |
| early_bulk | alt | 1048576 | 2.656 [2.607,2.708] (3) | 2.600 [2.562,2.648] (5) | +0.055 | 0.071 |
| early_bulk | alt | 4194304 | 7.597 [7.589,7.621] (3) | 4.850 [4.837,5.011] (5) | +2.746 | 0.036 |
| early_bulk | random | 1048576 | 3.530 [3.509,3.625] (3) | 3.165 [3.156,3.226] (5) | +0.365 | 0.036 |
| early_bulk | inverted | 32768 | 1.891 [1.891,1.891] (1) | 2.082 [2.082,2.082] (1) | -0.191 | - |
| early_bulk | inverted | 100000 | 1.925 [1.893,1.932] (3) | 4.106 [4.067,4.129] (5) | -2.181 | 0.036 |
| early_bulk | inverted | 262144 | 2.051 [2.001,2.080] (3) | 19.204 [19.080,19.386] (3) | -17.153 | 0.100 |
| early_bulk | inverted | 524288 | 2.262 [2.211,2.349] (3) | 71.705 [71.705,71.705] (1) | -69.443 | - |
| early_nodefree | none | 0 | 1.863 [1.855,1.881] (3) | 1.848 [1.828,1.884] (3) | +0.015 | 0.700 |
| early_nodefree | alt | 1048576 | 2.622 [2.616,2.646] (3) | 2.600 [2.562,2.648] (5) | +0.022 | 0.250 |
| early_nodefree | alt | 4194304 | 6.117 [6.072,6.470] (3) | 4.850 [4.837,5.011] (5) | +1.267 | 0.036 |
| early_nodefree | random | 1048576 | 3.365 [3.340,3.375] (3) | 3.165 [3.156,3.226] (5) | +0.199 | 0.036 |
| early_nodefree | inverted | 100000 | 4.152 [4.080,4.172] (3) | 4.106 [4.067,4.129] (5) | +0.046 | 0.393 |
| early_nodefree | inverted | 262144 | 19.393 [19.072,19.429] (3) | 19.204 [19.080,19.386] (3) | +0.189 | 0.700 |
| early_batchinit | none | 0 | 1.882 [1.829,1.889] (3) | 1.848 [1.828,1.884] (3) | +0.034 | 0.700 |
| early_batchinit | alt | 1048576 | 2.862 [2.813,3.042] (3) | 2.600 [2.562,2.648] (5) | +0.262 | 0.036 |
| early_batchinit | alt | 4194304 | 9.286 [9.016,9.428] (3) | 4.850 [4.837,5.011] (5) | +4.436 | 0.036 |
| early_batchinit | random | 1048576 | 3.767 [3.729,3.964] (3) | 3.165 [3.156,3.226] (5) | +0.601 | 0.036 |
| early_batchinit | inverted | 100000 | 4.116 [4.102,4.133] (3) | 4.106 [4.067,4.129] (5) | +0.010 | 0.393 |
| early_batchinit | inverted | 262144 | 19.321 [19.022,19.458] (3) | 19.204 [19.080,19.386] (3) | +0.117 | 1.000 |
| early_opt | none | 0 | 1.873 [1.847,1.890] (5) | 1.848 [1.828,1.884] (3) | +0.025 | 0.393 |
| early_opt | contig | 8388608 | 1.997 [1.954,2.044] (5) | 3.909 [3.902,3.996] (5) | -1.912 | 0.008 |
| early_opt | alt | 1048576 | 2.415 [2.394,2.420] (5) | 2.600 [2.562,2.648] (5) | -0.185 | 0.008 |
| early_opt | alt | 4194304 | 4.034 [4.018,4.157] (5) | 4.850 [4.837,5.011] (5) | -0.816 | 0.008 |
| early_opt | random | 1048576 | 2.964 [2.931,2.982] (5) | 3.165 [3.156,3.226] (5) | -0.201 | 0.008 |
| early_opt | mixed | 32768 | 1.807 [1.801,1.862] (5) | 2.057 [2.050,2.076] (5) | -0.250 | 0.008 |
| early_opt | inverted | 10000 | 1.885 [1.857,1.895] (5) | 1.857 [1.857,1.857] (1) | +0.027 | - |
| early_opt | inverted | 32768 | 1.886 [1.878,1.904] (5) | 2.082 [2.082,2.082] (1) | -0.196 | - |
| early_opt | inverted | 100000 | 1.930 [1.895,1.961] (5) | 4.106 [4.067,4.129] (5) | -2.176 | 0.008 |
| early_opt | inverted | 262144 | 2.005 [1.972,2.042] (5) | 19.204 [19.080,19.386] (3) | -17.199 | 0.036 |
| early_opt | inverted | 524288 | 2.135 [2.125,2.145] (5) | 71.705 [71.705,71.705] (1) | -69.570 | - |

## Worst case per mode (highest median TTI, or timeout lower bound)

| mode | pattern | count | TTI | floor (none/0) | overhead vs floor s |
|---|---|---|---|---|---|
| baseline | inverted | 524288 | 71.705 [71.705,71.705] (1) | 1.848 | 69.858 |
| early | inverted | 524288 | 71.100 [71.100,71.100] (1) | 1.854 | 69.246 |
| early_noinit1 | inverted | 524288 | 70.074 [70.074,70.074] (1) | 1.859 | 68.215 |
| early_coalesce | inverted | 262144 | 19.247 [19.118,19.518] (3) | 1.866 | 17.381 |
| early_presize | inverted | 262144 | 19.287 [18.877,19.478] (3) | 1.864 | 17.423 |
| early_bulk | inverted | 4194304 | 8.701 [8.692,9.008] (3) | 1.855 | 6.846 |
| early_nodefree | inverted | 262144 | 19.393 [19.072,19.429] (3) | 1.863 | 17.530 |
| early_batchinit | inverted | 262144 | 19.321 [19.022,19.458] (3) | 1.882 | 17.439 |
| early_opt | inverted | 4194304 | 4.082 [4.023,4.103] (3) | 1.873 | 2.209 |

## Largest same-session comparison point per pattern

Overhead = mode median TTI - same mode none/0 median TTI.  Ratios use medians of this session only.

| pattern | count | mode | TTI | overhead vs floor s | x early | x baseline |
|---|---|---|---|---|---|---|
| contig | 8388608 | baseline | 3.909 [3.902,3.996] (5) | 2.061 | 1.316 | 1.000 |
| contig | 8388608 | early | 2.969 [2.962,3.026] (5) | 1.115 | 1.000 | 0.760 |
| contig | 8388608 | early_coalesce | 2.400 [2.399,2.424] (3) | 0.534 | 0.808 | 0.614 |
| contig | 8388608 | early_opt | 1.997 [1.954,2.044] (5) | 0.124 | 0.672 | 0.511 |
| alt | 4194304 | baseline | 4.850 [4.837,5.011] (5) | 3.003 | 0.515 | 1.000 |
| alt | 4194304 | early | 9.420 [9.279,9.599] (5) | 7.566 | 1.000 | 1.942 |
| alt | 4194304 | early_noinit1 | 9.144 [8.990,9.208] (3) | 7.285 | 0.971 | 1.885 |
| alt | 4194304 | early_coalesce | 9.390 [9.288,9.418] (3) | 7.524 | 0.997 | 1.936 |
| alt | 4194304 | early_presize | 9.414 [9.169,9.428] (3) | 7.550 | 0.999 | 1.941 |
| alt | 4194304 | early_bulk | 7.597 [7.589,7.621] (3) | 5.741 | 0.806 | 1.566 |
| alt | 4194304 | early_nodefree | 6.117 [6.072,6.470] (3) | 4.255 | 0.649 | 1.261 |
| alt | 4194304 | early_batchinit | 9.286 [9.016,9.428] (3) | 7.404 | 0.986 | 1.915 |
| alt | 4194304 | early_opt | 4.034 [4.018,4.157] (5) | 2.161 | 0.428 | 0.832 |
| random | 1048576 | baseline | 3.165 [3.156,3.226] (5) | 1.318 | 0.815 | 1.000 |
| random | 1048576 | early | 3.886 [3.814,4.015] (5) | 2.032 | 1.000 | 1.228 |
| random | 1048576 | early_noinit1 | 3.877 [3.735,4.039] (3) | 2.018 | 0.998 | 1.225 |
| random | 1048576 | early_coalesce | 3.856 [3.773,4.015] (3) | 1.991 | 0.992 | 1.218 |
| random | 1048576 | early_presize | 3.892 [3.851,4.081] (3) | 2.028 | 1.002 | 1.229 |
| random | 1048576 | early_bulk | 3.530 [3.509,3.625] (3) | 1.675 | 0.908 | 1.115 |
| random | 1048576 | early_nodefree | 3.365 [3.340,3.375] (3) | 1.502 | 0.866 | 1.063 |
| random | 1048576 | early_batchinit | 3.767 [3.729,3.964] (3) | 1.885 | 0.969 | 1.190 |
| random | 1048576 | early_opt | 2.964 [2.931,2.982] (5) | 1.091 | 0.763 | 0.936 |
| mixed | 32768 | baseline | 2.057 [2.050,2.076] (5) | 0.210 | 0.967 | 1.000 |
| mixed | 32768 | early | 2.127 [2.108,2.156] (5) | 0.273 | 1.000 | 1.034 |
| mixed | 32768 | early_opt | 1.807 [1.801,1.862] (5) | -0.066 | 0.849 | 0.878 |
| inverted | 524288 | baseline | 71.705 [71.705,71.705] (1) | 69.858 | 1.009 | 1.000 |
| inverted | 524288 | early | 71.100 [71.100,71.100] (1) | 69.246 | 1.000 | 0.992 |
| inverted | 524288 | early_noinit1 | 70.074 [70.074,70.074] (1) | 68.215 | 0.986 | 0.977 |
| inverted | 524288 | early_bulk | 2.262 [2.211,2.349] (3) | 0.407 | 0.032 | 0.032 |
| inverted | 524288 | early_opt | 2.135 [2.125,2.145] (5) | 0.263 | 0.030 | 0.030 |
| inverted | 4194304 | early_bulk | 8.701 [8.692,9.008] (3) | 6.846 | - | - |
| inverted | 4194304 | early_opt | 4.082 [4.023,4.103] (3) | 2.209 | - | - |

## Correctness (verify rows, fill=1, kho_bench full-content verify)

| mode | pattern | count | status | verify | check | log |
|---|---|---|---|---|---|---|
| early_opt | inverted | 2097152 | ok | PASS | - | verify/raw/early_opt_inverted_2097152_run1.log |
| early_opt | alt | 4194304 | ok | PASS | - | verify/raw/early_opt_alt_4194304_run1.log |
| early_opt | random | 1048576 | ok | PASS | - | verify/raw/early_opt_random_1048576_run1.log |
| early_opt | contig | 8388608 | ok | PASS | - | verify/raw/early_opt_contig_8388608_run1.log |
| early_opt | mixed | 32768 | ok | PASS | - | verify/raw/early_opt_mixed_32768_run1.log |
| early_bulk | inverted | 2097152 | ok | PASS | - | verify/raw/early_bulk_inverted_2097152_run1.log |
| early_bulk | alt | 4194304 | ok | PASS | - | verify/raw/early_bulk_alt_4194304_run1.log |
| early_noinit1 | inverted | 262144 | ok | PASS | - | verify/raw/early_noinit1_inverted_262144_run1.log |
| early_noinit1 | alt | 4194304 | ok | PASS | - | verify/raw/early_noinit1_alt_4194304_run1.log |
| early_coalesce | inverted | 262144 | ok | PASS | - | verify/raw/early_coalesce_inverted_262144_run1.log |
| early_coalesce | alt | 4194304 | ok | PASS | - | verify/raw/early_coalesce_alt_4194304_run1.log |
| early_coalesce | contig | 8388608 | ok | PASS | - | verify/raw/early_coalesce_contig_8388608_run1.log |
| early_presize | inverted | 262144 | ok | PASS | - | verify/raw/early_presize_inverted_262144_run1.log |
| early_presize | alt | 4194304 | ok | PASS | - | verify/raw/early_presize_alt_4194304_run1.log |
| early_nodefree | inverted | 262144 | ok | PASS | - | verify/raw/early_nodefree_inverted_262144_run1.log |
| early_nodefree | alt | 4194304 | ok | PASS | - | verify/raw/early_nodefree_alt_4194304_run1.log |
| early_batchinit | inverted | 262144 | ok | PASS | - | verify/raw/early_batchinit_inverted_262144_run1.log |
| early_batchinit | alt | 4194304 | ok | PASS | - | verify/raw/early_batchinit_alt_4194304_run1.log |
