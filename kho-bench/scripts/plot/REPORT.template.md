# KHO early memblock reservation: benchmark report

{{HEADER}}

All measured numbers in this file come from the generated fragments. The prose
around them has no measured numbers. Two datasets are combined: **m2** (M2 sweep,
modes `baseline` and `early`) and **m3** (M3 sweep, the improvement modes plus
same-session `baseline`/`early` references). They are separate builds and separate
sweeps. A mode name that exists in both is shown as `m2/<mode>` and `m3/<mode>` and
is never merged; the improvement modes exist only in m3 and keep their plain names.
Comparisons in the tables are made within one dataset unless they are explicitly
labelled "across datasets".

## 1. Conclusion

{{CONCLUSION}}

**What this means:**
- The idea is **workable in this VM with small improvements**. The best mode's slowest
  measured point, over every pattern and count, is a few seconds above the
  no-preservation floor (bullets above).
- Without the improvements it is **not workable for the worst-case layout**. Plain
  `early` scales like `baseline` on `inverted`, because both insert one memblock
  region per preserved range into a sorted array. Measured times grow from seconds
  to many minutes over the swept counts.
- The decisive improvement for the worst case is building the reservations as
  sorted runs and merging them in bulk (`early_bulk`, included in `early_opt`). It
  removes the per-insert memmove that makes `inverted` quadratic (walk1 timer in
  section 7). On `alt` and `random`, which have no ordering problem, the largest
  single gain comes from not freeing the radix node pages against millions of
  regions (`early_nodefree`). `early_coalesce` helps only `contig`. The other single
  changes are small. Only the combination (`early_opt`) beats same-session
  `baseline` on every pattern. Section 7 has the per-mode numbers.
- The best mode's highest point and the next pattern's highest point may be within
  noise of each other (see the overlap statement above). In that case, read the
  per-pattern ranking for the best mode as "highest median", not as a separated
  worst case.

## 2. Summary

{{SUMMARY}}

## 3. Method

- **Kernel A** is cold-booted in the VM with the user's `mk r`. It creates
  preservations with the built-in bench generator (`kho_bench.pattern=`,
  `kho_bench.count=`; patterns contig, alt, random, mixed, inverted). It then kexecs
  into **kernel B**, which is the same image.
- Kernel B selects the restore path at runtime with `kho_reserve_mode=`:
  - `baseline` is the upstream flow: bootmem-only mode, then
    `kho_preserved_memory_reserve()` per key in `kho_memory_init`.
  - `early` is the prototype:
    - right after memblock resizing is allowed, walk #1 runs `memblock_reserve()` on
      every preserved range and radix node page, and bootmem-only mode is turned off;
    - after memmap init, walk #2 initialises the struct pages;
    - the radix node pages are then freed.
  - The M3 improvement modes are `early` plus one change each (`early_noinit1`,
    `early_coalesce`, `early_presize`, `early_bulk`, `early_nodefree`,
    `early_batchinit`), and `early_opt` is all of them combined. Their definitions
    are in `kho-bench/results/m3/summary.md`.
- **Time to init (TTI)** is the printk timestamp of the last `Run /sbin/init as init
  process` in kernel B's part of the serial log. Timestamps restart at kexec.
- Each row of `data.csv` is one measured boot. A point (dataset, mode, pattern,
  count) is the median of its `status=ok` rows, with min/max as the error bars.
- Rows that are not plain `ok` are handled as follows:
  - A run that is flagged but still has an init line in its serial log is shown as a
    separate flagged value.
  - A timed-out run with no init line would be shown only as a lower bound.
  - A run where kernel A could not generate the layout (`generate_failed`, ENOMEM)
    has no kernel B boot. It is marked at its count without a time.
- Nothing is fitted, extrapolated or simulated. Every plotted value is a measured VM
  boot.
- Correctness (preserved data intact after kexec) was checked by separate verify
  boots that are not part of the timing data. See the "Correctness" sections of
  `kho-bench/results/m2/summary.md` and `kho-bench/results/m3/summary.md`.

{{RUNS}}

## 4. Environment

{{ENV}}

## 5. Results (graphs)

{{GRAPHS}}

### Per-pattern tables (median TTI in s, [min–max] of ok runs, ok/rows)

{{TTI_TABLES}}

## 6. Worst case

{{WORST}}

Mechanism (from M2 analysis, `kho-bench/results/m2/summary.md`, "Why"):
- The radix walk visits higher orders first, so the high-address blocks are reserved
  first.
- Each later lower-address reserve lands in front of them, and
  `memblock_insert_region()` memmoves most of the reserved array.
- The reserve phase is nearly all of kernel B's TTI at large counts (see the
  restore-phase graph). `early_bulk`/`early_opt` replace these inserts with sorted
  bulk merges.

## 7. Improvement effects per mode

{{IMPROVEMENTS}}

## 8. Is it workable?

Headline modes vs `baseline`/`early` of the same dataset, per pattern at the largest
count measured in both (generated):

{{WORKABLE}}

Assessment:
- The evidence is in the conclusion bullets and the table above:
  - the best mode stays within a few seconds of its count-0 floor on every
    pattern;
  - on the large-count points it is faster than the same-session `baseline`;
  - on `inverted` it is more than an order of magnitude faster than same-session
    `baseline`/`early`. Against m2 at larger counts the gap is larger still, but
    that comparison is across datasets.
- Its remaining cost at the largest counts is mostly `memblock_free_all` walking
  millions of reserved regions. `baseline` pays the same cost at equal region
  counts (restore-phase graph; M3 summary "What remains").
- It needs a temporary range array in KHO scratch during walk #1, on top of the
  `memblock.reserved` array growth that every mode has. Shrinking scratch motivated
  this idea, so this is a real cost. The sizes are in the M3 summary ("Extra KHO
  scratch memory"); they are derived from logged counts, not measured.

## 9. Caveats

Generated from the data (flagged runs, rows without a TTI, points with few runs):

{{FLAGS}}

Carried over from M2 (`.agents/teamwork/sub_orch_m2/handoff.md`, "Caveats"):
- Noise at the slow end. The host is a nested cloud VM. A challenger re-run of
  baseline inverted at 1M fell clearly outside M2's narrow range, so the n=3 spreads
  understate the real variance.
- `mixed` grows just as quadratically per key as `inverted` in `baseline`/`early`. It
  is capped by ENOMEM in kernel A at a much smaller count, so its curve stops early.
  Layouts with several stacked inverted orders could be worse; they were not
  measured.
- The flagged `m2/baseline` inverted 2M run is a measured TTI taken from the serial
  log:
  - the host's ssh wait (B_TIMEOUT) ran out before kernel B reached init;
  - but the serial log, captured until the VM was killed, contains kernel B's init
    line (see its `.harness.log`).

  It is drawn as a flagged value, not as a lower bound.
- The x axis is the generator's `kho_bench.count`. The number of radix keys depends
  on the pattern. See the `keys` column in `data.csv`.

Carried over from M3 (`kho-bench/results/m3/summary.md`, `.agents/teamwork/m3_auditor_1/handoff.md`,
`.agents/teamwork/m3_challenger_1_1/handoff.md`):
- **Different builds.** m2 and m3 are different kernel builds and sweeps (see
  Environment). Cross-dataset ratios are labelled as such. Same-session m3
  `baseline`/`early` references at matching points agree closely with m2 (compare
  the `m2/` and `m3/` columns in the tables).
- **No same-session slow references at the largest counts.** M3 measured
  `baseline`/`early` on `inverted` only up to a moderate count, some of it with n=1.
  The minutes-long `inverted` points at 1M and 2M exist only in m2.
- **Single-change modes stop early.** The five per-key single modes were swept only
  up to `inverted` 262144 and on a reduced set of patterns and counts. `early_bulk`
  and `early_opt` go up to the largest counts.
- **Largest `inverted` count.** The `early_opt` attempt at the next doubling failed
  in kernel A (`generate_failed`, ENOMEM). No kernel B boot took place, and no time
  is reported for it.
- **Deferred struct page init is inactive in this VM.** `CONFIG_MEM_ALLOC_PROFILING_DEBUG`
  makes `defer_init()` return false, so walk #2 only checks pages. The walk2 and
  `early_batchinit` numbers do not carry over to configurations with deferred init.
- **`early_opt` worst pattern vs `alt`.** The auditor noted that `early_opt` inverted
  at its largest count overlaps `alt` at its largest count (min–max ranges). The
  overlap test is in the conclusion.
- **Fallback coverage.** The bulk-walk fallback was exercised only for a failure
  before any range was inserted; a failure part-way through the merge is untested.
- **`early_noinit1` semantics** differ from M1's reserve + mark_noinit on overlapping
  ranges. This never triggered (no warnings, all checks and verifies passed), but it
  is a known latent difference.
- **TTI below the floor** at `early_opt` mixed: `mixed` preserves a large amount of
  memory, so `memblock_free_all` has less free memory to release. That is less work,
  not negative overhead.
- **n per point** varies (runs-per-point list in the Method section). Points with
  fewer than 3 ok runs are marked with a ring and `n=k` in the graphs and are listed
  above.

## 10. Untested areas

- A walk #1 bulk-merge failure part-way through (only the pre-insertion failure
  fallback was exercised).
- Deferred struct page init (inactive in this VM), so walk #2's real init loop and
  `early_batchinit`'s init path are unmeasured.
- Preserved ranges below 4 GB, multiple memory zones.
- NUMA (the VM has a single node).
- Architectures other than x86.
- Counts above the largest count kernel A could generate per pattern. The ENOMEM
  probes show only whether kernel A could generate that many blocks; kernel B was
  never booted there.
- Real memfd/LUO preservations. The bench generator stands in for them.
- Bare-metal hosts. All numbers are from a nested cloud VM.

## 11. Data / traceability

{{TRACE}}

- Raw serial logs and harness logs: `kho-bench/results/m2/raw/` and
  `kho-bench/results/m3/raw/` (`<mode>_<pattern>_<count>_run<k>.log` and
  `.harness.log`).
- Script-generated sweep summaries with phase breakdowns and significance tests:
  `kho-bench/results/m2/summary.md`, `kho-bench/results/m3/summary.md`.
- Pipeline and regeneration instructions: `kho-bench/scripts/plot/README.md`.
