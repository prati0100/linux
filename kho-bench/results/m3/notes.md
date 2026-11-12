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
