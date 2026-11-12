#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# probe_max.sh - empirical capacity probe: for each pattern, double the count
# from a start value until a run fails (generate fails, B does not boot, ...)
# or the cap is reached.  Each probe is a real VM boot via run_point.sh
# --probe (results in results/probe/, not data.csv).
# Usage: probe_max.sh <variant> <pattern>:<start>[:<cap>] ...
#   e.g. probe_max.sh early alt:262144 mixed:16384:1048576
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
V=$1; shift
export B_TIMEOUT=${B_TIMEOUT:-600} GEN_TIMEOUT=${GEN_TIMEOUT:-600}
for spec in "$@"; do
	IFS=: read -r p n cap <<<"$spec"
	cap=${cap:-33554432}
	while [ "$n" -le "$cap" ]; do
		echo "=== probe $V $p $n"
		if "$SCRIPT_DIR/run_point.sh" --probe "$V" "$p" "$n" 1 2>&1 | grep -E "finished|csv:|generate rc"; then :; fi
		st=$(tail -1 "$SCRIPT_DIR/../results/probe/probe.csv" | cut -d, -f10)
		cnt=$(tail -1 "$SCRIPT_DIR/../results/probe/probe.csv" | cut -d, -f3)
		[ "$cnt" = "$n" ] || st=no_row
		echo "=== result $V $p $n status=$st"
		[ "$st" = ok ] || break
		n=$((n * 2))
	done
done
