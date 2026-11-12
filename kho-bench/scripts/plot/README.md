# KHO bench graphs + report pipeline

This pipeline reads the harness `data.csv` files **directly**. Each row is one
measured VM boot. The pipeline:
- re-derives every row's time to init (TTI) from its raw serial log, and refuses to
  plot if any row disagrees;
- recomputes per-point median/min/max itself;
- writes the graphs, one CSV per graph listing exactly the plotted points, and
  `kho-bench/REPORT.md`.

There are no hand-typed numbers, no fits, and no extrapolation.

## Regenerate (from the repo root)

```
python3 kho-bench/scripts/plot/kho_plot.py \
    --data kho-bench/results/m2/data.csv \
    --data kho-bench/results/m3/data.csv \
    --out kho-bench/graphs \
    --report kho-bench/REPORT.md
```

This is the final (M4) command: M2 and M3 together. Optionally pass
`--modes ...` to choose and order the series.
- If a variant name appears in more than one data.csv (for example, M3 re-measures
  `baseline`), its series are named `<label>/<variant>`, e.g. `m2/baseline` and
  `m3/baseline`.
- The label is the name of the data.csv's directory. Override it with
  `--data path:label`.
- Datasets are never merged. Every CSV row has a `dataset` column (m2/m3) and a
  `data_csv` column. When a panel is coloured by mode, an older dataset's series
  (e.g. `m2/early`) is drawn with dashed lines and hollow markers, and the newest
  dataset's series with solid lines and filled markers.
- Report comparisons (mode vs baseline/early, improvement effects) use references
  **from the same dataset** only. The few cross-dataset ratios in the conclusion are
  labelled "across datasets".
- **Best mode** (computed): at the largest worst-case-pattern count where at least
  two improvement modes have ok runs, the improvement mode with the lowest median.
  **Headline modes**: for `baseline` and `early`, the dataset with the most ok
  points, plus the best mode.

Other options:
- `--patterns ...`: subset/order of the patterns.
- `--floor-pattern none`: the count-0 reference.
- `--filter col=value`: keep only matching rows. Repeatable; for example
  `--filter bzimage_sha=<sha>`.
- `--template`: the report template.

Tests (synthetic data only, writes only to a temp dir):

```
python3 kho-bench/scripts/plot/test_kho_plot.py
```

## Files

| file | role |
|---|---|
| `kho_plot.py` | CLI and graphs |
| `khoplot_data.py` | loads data.csv, re-checks the raw logs, classifies rows, aggregates |
| `kho_report.py` | collects the report numbers (`report_numbers.json`) and renders `REPORT.template.md` |
| `REPORT.template.md` | report prose with `{{PLACEHOLDERS}}`. The prose has no measured numbers; every number comes from a generated fragment |
| `test_kho_plot.py` | unit and end-to-end tests on synthetic data |

## Row classes (per data.csv row)

| class | rule | drawn as |
|---|---|---|
| ok | `status == ok` and `time_to_init_s` set | part of the median/min/max |
| flagged | `time_to_init_s` set, `status != ok` (e.g. `b_ssh_timeout` with an init line in the serial log) | yellow X, a measured value. Not in the ok stats and **not** a lower bound |
| lower_bound | no `time_to_init_s`, status contains `b_ssh_timeout` | ^ with an up arrow at `b_last_ts_s` (kernel B's last printk). TTI > that value |
| failed | anything else without a TTI (e.g. `generate_failed`: ENOMEM in kernel A, no kernel B boot) | a crimson x at the bottom of the axes at its count, with a label, and **no time value**. CSV `point_kind=failed_no_tti` with an empty `plotted_value` |

## Outputs (`--out`)

Each PNG has a CSV with the same basename, holding one row per drawn point. The
columns are:

```
graph, panel, dataset, pattern, mode, variant, count, metric, unit, point_kind,
n_rows, n_runs, median, min, max, plotted_value, flags, data_csv, raw_logs
```

- `point_kind` is one of `median` (2+ ok runs), `single_run`, `flagged_measured`,
  `lower_bound`, `failed_no_tti`, `floor_reference_line` (the count-0 horizontal
  line from pattern `none`), or `bar_median`/`bar_single_run` (bar panel of
  `tti_worst_case`).
- `raw_logs` is a space-separated list of the serial logs behind the point,
  relative to the repo root. Each log has a sibling `.harness.log`.

| graph | content |
|---|---|
| `tti_vs_count` | Headline: TTI vs `kho_bench.count`, log-log, all patterns, headline modes (baseline, early, best mode), with count-0 floor lines. The worst-case pattern (highest measured TTI) is highlighted and annotated |
| `tti_vs_count_by_pattern` | One panel per pattern, **every** mode of every dataset (colour = mode). The worst-case panel is framed in red |
| `tti_worst_case` | Worst-case pattern only: every mode vs count (left); bars of every mode of the best mode's dataset at the count measured by the most modes (right) |
| `tti_best_mode` | Best mode across all patterns (scaling), with its count-0 floor |
| `restore_phases_vs_count` | Headline modes. Kernel B restore timers in ms: early walk1/walk2/free_nodes; baseline discover/retrieve; memblock_free_all for both. Zero medians are not drawn (log axis) and are not in the CSV |
| `reserved_cnt_vs_count` | Headline modes. `memblock.reserved.cnt` after the KHO reservations (`memblock_reserved_regions`) |
| `report_numbers.json` | Every number that the report uses |
