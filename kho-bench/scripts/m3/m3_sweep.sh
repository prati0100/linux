#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# m3_sweep.sh - resumable M3 sweep (all improvement modes + baseline/early
# reference on the same image).  Thin wrapper around scripts/m3/kho_sweep.py
# with the M3 plan and output dir.
#
# Usage: kho-bench/scripts/m3/m3_sweep.sh [--dry-run] [--deadline-hours H]
#                                          [--max-extra-fails N]
# Background launch (re-running the same command resumes):
#   setsid nohup kho-bench/scripts/m3/m3_sweep.sh </dev/null \
#       >>kho-bench/results/m3/sweep.out 2>&1 &
# Graceful stop: touch kho-bench/results/m3/STOP
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BENCH_DIR=$(dirname "$(dirname "$SCRIPT_DIR")")
mkdir -p "$BENCH_DIR/results/m3"
export GEN_TIMEOUT=${GEN_TIMEOUT:-900}
exec python3 "$SCRIPT_DIR/kho_sweep.py" --plan "$SCRIPT_DIR/m3_plan.tsv" \
	--outdir "$BENCH_DIR/results/m3" "$@"
