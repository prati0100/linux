#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# m2_sweep.sh - resumable M2 sweep (baseline vs early, all patterns).
# Thin wrapper around kho_sweep.py with the M2 plan and output dir.
#
# Usage: kho-bench/scripts/m2_sweep.sh [--dry-run] [--deadline-hours H]
#                                       [--max-extra-fails N]
# Background launch (re-running the same command resumes):
#   setsid nohup kho-bench/scripts/m2_sweep.sh </dev/null \
#       >>kho-bench/results/m2/sweep.out 2>&1 &
# Graceful stop: touch kho-bench/results/m2/STOP
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BENCH_DIR=$(dirname "$SCRIPT_DIR")
exec python3 "$SCRIPT_DIR/kho_sweep.py" --plan "$SCRIPT_DIR/m2_plan.tsv" \
	--outdir "$BENCH_DIR/results/m2" "$@"
