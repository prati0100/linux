#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run_point.sh - measure KHO restore (time to init in the kexec'd kernel)
# for one (variant, pattern, count) point, <runs> times.  Each run is a
# fresh cold boot of the VM via `mk r` (kernel A), preservation generation
# with the built-in kho_bench, a kexec into kernel B (same bzImage) with
# kho_reserve_mode=<variant> appended, and a full serial capture.
#
# Usage (from anywhere; the kernel tree is located from this script):
#   kho-bench/scripts/m3/run_point.sh [options] <variant> <pattern> <count> <runs> [k=v ...]
#     k=v           extra kho_bench module params (order=, stride=, max_order=,
#                   seed=, fill=) applied in kernel A
#   options:
#     --verify      run the kho_bench verifier in B after boot (after init;
#                   not part of time-to-init)
#     --no-free-hugepages  keep the 50x1G boot hugepages in A
#     --b-cmdline nohp|reuse  kernel B cmdline (default nohp): nohp builds
#                   B's cmdline from A's /proc/cmdline with all
#                   hugepages/hugepagesz/default_hugepagesz params removed
#                   (B must log "pre-allocated 0 pages"); reuse = M1
#                   behaviour (--reuse-cmdline, B inherits hugepages=50)
#     --outdir DIR  write logs to DIR/raw and rows to DIR/data.csv
#                   (default: results/m3/raw + results/m3/data.csv)
#     --extra-cmdline "S"  extra kernel A cmdline (inherited by B)
#     --probe       write logs/CSV under results/m3/probe/ instead of
#                   results/m3/raw + results/m3/data.csv (probes etc.)
#     --no-kexec    stop after A (generate if count>0); for investigations,
#                   implies --probe
#     --keep-going  continue with the next run after a failed run (default)
#     --stop-on-fail  stop after the first failed run
#   env timeouts (seconds): A_SSH_TIMEOUT=300 GEN_TIMEOUT=900
#     KEXEC_LOAD_TIMEOUT=180 B_TIMEOUT=1500 VERIFY_TIMEOUT=900
#     POWEROFF_TIMEOUT=120
#   exit: 0 all runs ok, 1 some run failed, 2 usage, 3 refused (VM lock
#     held or a qemu-system-x86 already running; nothing was run)
#
# Output: results/m3/raw/<variant>_<pattern>_<count>_run<k>.log (full serial
# log of A and B) + .harness.log (ssh side outputs), one row per run appended
# to results/m3/data.csv.  Run index k = first index whose log file does not
# exist yet, claimed atomically (noclobber), so logs are never overwritten.
#
# The harness never builds: it boots arch/x86/boot/bzImage as built by `mk`
# and copies it to the VM (/root/bzImage, used by kexec) only when the
# sha256 differs.  Only one VM may run at a time (flock + qemu check).

set -u
set -o pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# M3 copy: lives in kho-bench/scripts/m3/, writes to kho-bench/results/m3/
BENCH_DIR=$(dirname "$(dirname "$SCRIPT_DIR")")
TREE=$(dirname "$BENCH_DIR")
LOCK=${KHO_BENCH_LOCK:-/tmp/kho-bench-vm.lock}

A_SSH_TIMEOUT=${A_SSH_TIMEOUT:-300}
GEN_TIMEOUT=${GEN_TIMEOUT:-900}
KEXEC_LOAD_TIMEOUT=${KEXEC_LOAD_TIMEOUT:-180}
B_TIMEOUT=${B_TIMEOUT:-1500}
VERIFY_TIMEOUT=${VERIFY_TIMEOUT:-900}
POWEROFF_TIMEOUT=${POWEROFF_TIMEOUT:-120}
QEMU_TIMEOUT=$((A_SSH_TIMEOUT + GEN_TIMEOUT + KEXEC_LOAD_TIMEOUT + B_TIMEOUT + VERIFY_TIMEOUT + POWEROFF_TIMEOUT + 300))

VERIFY=0 FREE_HP=1 EXTRA="" PROBE=0 NOKEXEC=0 STOP_ON_FAIL=0 B_CMDLINE=nohp OUTBASE=""
usage() { sed -n '/^# Usage/,/^# Output/p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
while [ $# -gt 0 ]; do
	case "$1" in
	--verify) VERIFY=1 ;;
	--no-free-hugepages) FREE_HP=0 ;;
	--b-cmdline) B_CMDLINE=$2; shift ;;
	--outdir) OUTBASE=$2; shift ;;
	--extra-cmdline) EXTRA=$2; shift ;;
	--probe) PROBE=1 ;;
	--no-kexec) NOKEXEC=1; PROBE=1 ;;
	--keep-going) STOP_ON_FAIL=0 ;;
	--stop-on-fail) STOP_ON_FAIL=1 ;;
	-h|--help) usage ;;
	--*) echo "unknown option $1" >&2; usage ;;
	*) break ;;
	esac
	shift
done
[ $# -ge 4 ] || usage
VARIANT=$1 PATTERN=$2 COUNT=$3 RUNS=$4; shift 4
PARAMS=("$@")
for kv in "${PARAMS[@]}"; do
	[[ "$kv" =~ ^(order|stride|max_order|seed|fill)=[0-9A-Za-z]+$ ]] ||
		{ echo "bad kho_bench param '$kv'" >&2; exit 2; }
done
[[ "$VARIANT" =~ ^[a-z0-9_]+$ && "$PATTERN" =~ ^[a-z]+$ && "$COUNT" =~ ^[0-9]+$ && "$RUNS" =~ ^[0-9]+$ ]] ||
	{ echo "bad arguments" >&2; usage; }
[[ "$B_CMDLINE" = nohp || "$B_CMDLINE" = reuse ]] ||
	{ echo "bad --b-cmdline '$B_CMDLINE'" >&2; usage; }

if [ $PROBE = 1 ]; then
	OUTDIR=$BENCH_DIR/results/m3/probe; CSV=$OUTDIR/probe.csv
elif [ -n "$OUTBASE" ]; then
	mkdir -p "$OUTBASE" || exit 1
	OUTBASE=$(cd "$OUTBASE" && pwd)
	OUTDIR=$OUTBASE/raw; CSV=$OUTBASE/data.csv
else
	OUTDIR=$BENCH_DIR/results/m3/raw; CSV=$BENCH_DIR/results/m3/data.csv
fi
mkdir -p "$OUTDIR"

cd "$TREE" || exit 1
[ -f arch/x86/boot/bzImage ] || { echo "no bzImage; build with mk first" >&2; exit 1; }

# ---- single-VM guard -------------------------------------------------------
exec 9>>"$LOCK"
flock -n 9 || { echo "another kho-bench VM run holds $LOCK" >&2; exit 3; }
if pgrep -x qemu-system-x86 >/dev/null; then
	echo "a qemu-system-x86 is already running; refusing:" >&2
	pgrep -ax qemu-system-x86 >&2; exit 3
fi

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=3 -o ServerAliveInterval=5 -o ServerAliveCountMax=3)
# vmssh <timeout> <cmd>
vmssh() { local t=$1; shift; timeout -k 5 "$t" ssh "${SSH_OPTS[@]}" vm "$@"; }

GIT_DESC=$(git describe --always --dirty 2>/dev/null || echo unknown)
LOCAL_SHA=$(sha256sum arch/x86/boot/bzImage | cut -d' ' -f1)

MKPID="" SID=""
log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$HLOG"; }

stop_vm() {
	[ -n "$MKPID" ] || return 0
	if kill -0 "$MKPID" 2>/dev/null; then
		vmssh 20 poweroff >/dev/null 2>&1
		local end=$((SECONDS + POWEROFF_TIMEOUT))
		while kill -0 "$MKPID" 2>/dev/null && [ $SECONDS -lt $end ]; do sleep 1; done
	fi
	if kill -0 "$MKPID" 2>/dev/null; then
		log "VM did not power off in ${POWEROFF_TIMEOUT}s; killing our qemu (session $SID)"
		pkill -TERM -s "$SID" -x qemu-system-x86; sleep 5
		pkill -KILL -s "$SID" 2>/dev/null
		sleep 1
	fi
	wait "$MKPID" 2>/dev/null
	MKPID=""
	if pgrep -x qemu-system-x86 >/dev/null; then
		log "ERROR: qemu still running after stop"; pgrep -ax qemu-system-x86 | tee -a "$HLOG"
		return 1
	fi
	return 0
}
trap 'stop_vm; exit 130' INT TERM
trap 'stop_vm' EXIT

wait_ssh() {	# wait_ssh <timeout> <cmd-that-must-succeed>
	local end=$((SECONDS + $1)) cmd=$2
	while [ $SECONDS -lt $end ]; do
		kill -0 "$MKPID" 2>/dev/null || return 2
		vmssh 15 "$cmd" >/dev/null 2>&1 && return 0
		sleep 2
	done
	return 1
}

kv_get() { sed -n "s/^$1=//p" "$2" | tail -1; }

one_run() {
	local k=1 base
	while :; do
		base=$OUTDIR/${VARIANT}_${PATTERN}_${COUNT}_run$k
		# claim atomically; never overwrite an existing log
		if (set -o noclobber; : > "$base.log") 2>/dev/null; then break; fi
		k=$((k + 1))
	done
	LOG=$base.log HLOG=$base.harness.log RUNIDX=$k
	: > "$HLOG"
	local status="" verify="" a_memfree="" b_hugetlb="" b_memfree="" b_ssh_s="" b_dmesg_init=""
	local cmdline="$EXTRA kho_bench.pattern=$PATTERN kho_bench.count=$COUNT"
	for kv in "${PARAMS[@]}"; do cmdline="$cmdline kho_bench.$kv"; done
	log "run $k: variant=$VARIANT pattern=$PATTERN count=$COUNT params='${PARAMS[*]}' verify=$VERIFY git=$GIT_DESC bzImage=$LOCAL_SHA"
	log "EXTRA_CMDLINE=$cmdline"

	EXTRA_CMDLINE="$cmdline" setsid timeout -k 10 "$QEMU_TIMEOUT" mk r </dev/null >"$LOG" 2>&1 &
	MKPID=$!
	sleep 1
	SID=$(ps -o sid= -p "$MKPID" 2>/dev/null | tr -d ' ')
	[ -n "$SID" ] || SID=$MKPID

	while :; do	# single-pass block; break on failure
		local t0=$SECONDS
		if ! wait_ssh "$A_SSH_TIMEOUT" true; then status=a_ssh_timeout; break; fi
		log "A ssh up after $((SECONDS - t0))s"
		local a_boot_id
		a_boot_id=$(vmssh 15 cat /proc/sys/kernel/random/boot_id) || { status=a_ssh_error; break; }
		vmssh 15 'uname -a; cat /proc/cmdline' 2>&1 | tee -a "$HLOG"

		local vm_sha
		vm_sha=$(vmssh 60 'sha256sum /root/bzImage 2>/dev/null' | cut -d' ' -f1)
		if [ "$vm_sha" != "$LOCAL_SHA" ]; then
			log "VM /root/bzImage sha '$vm_sha' != local; mk cp"
			timeout 300 mk cp >>"$HLOG" 2>&1 || { status=mk_cp_failed; break; }
			vm_sha=$(vmssh 60 'sha256sum /root/bzImage' | cut -d' ' -f1)
			[ "$vm_sha" = "$LOCAL_SHA" ] || { status=mk_cp_mismatch; break; }
		fi

		log "A meminfo before hugepage free:"
		vmssh 30 'grep -E "MemTotal|MemFree|MemAvailable|Hugetlb|HugePages_Total" /proc/meminfo; echo nr_1G=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages)' 2>&1 | tee -a "$HLOG"
		if [ $FREE_HP = 1 ]; then
			vmssh 300 'echo 0 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages' 2>&1 | tee -a "$HLOG" ||
				{ status=hugepage_free_failed; break; }
			log "A meminfo after hugepage free:"
			vmssh 30 'grep -E "MemTotal|MemFree|MemAvailable|Hugetlb" /proc/meminfo; echo nr_1G=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages)' 2>&1 | tee -a "$HLOG"
			local nr1g
			nr1g=$(vmssh 15 'cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages')
			log "A nr_1G after free: '$nr1g'"
			[ "$nr1g" = 0 ] || { status=hugepage_free_incomplete; break; }
		fi
		a_memfree=$(vmssh 15 "awk '/^MemFree:/{print \$2}' /proc/meminfo")

		if [ "$COUNT" -gt 0 ]; then
			local setp="echo $PATTERN > /sys/module/kho_bench/parameters/pattern && echo $COUNT > /sys/module/kho_bench/parameters/count"
			for kv in "${PARAMS[@]}"; do setp="$setp && echo ${kv#*=} > /sys/module/kho_bench/parameters/${kv%%=*}"; done
			vmssh 30 "$setp" 2>&1 | tee -a "$HLOG" || { status=param_set_failed; break; }
			log "generate (timeout ${GEN_TIMEOUT}s)"
			local g0=$SECONDS
			vmssh "$GEN_TIMEOUT" 'echo 1 > /sys/kernel/debug/kho_bench/generate' >>"$HLOG" 2>&1
			local grc=$?
			log "generate rc=$grc wall=$((SECONDS - g0))s"
			vmssh 30 'cat /sys/kernel/debug/kho_bench/status; dmesg | grep -E "kho_bench: (generated|generate )"; grep -E "MemFree" /proc/meminfo' 2>&1 | tee -a "$HLOG"
			if [ $grc -ne 0 ] || ! vmssh 15 'grep -q "generated=1" /sys/kernel/debug/kho_bench/status'; then
				status="generate_failed(rc=$grc)"; break
			fi
			if ! vmssh 15 "dmesg | grep -q 'kho_bench: generated pattern=$PATTERN count=$COUNT '"; then
				status=generate_line_missing; break
			fi
		fi
		[ $NOKEXEC = 1 ] && { status=ok_no_kexec; vmssh 30 'dmesg | grep -E "WARNING|BUG" ' 2>&1 | tee -a "$HLOG"; break; }

		log "kexec load (timeout ${KEXEC_LOAD_TIMEOUT}s, b-cmdline=$B_CMDLINE)"
		if [ "$B_CMDLINE" = nohp ]; then
			local a_cmdline b_cmdline
			a_cmdline=$(vmssh 15 cat /proc/cmdline) || { status=a_ssh_error; break; }
			b_cmdline=$(printf '%s\n' "$a_cmdline" | sed -E \
				-e 's/(^| )(default_hugepagesz|hugepagesz|hugepages)=[^ ]*/ /g' \
				-e 's/  +/ /g; s/^ //; s/ $//')
			b_cmdline="$b_cmdline kho_reserve_mode=$VARIANT"
			case "$b_cmdline" in
			*\'*|*hugepages*) log "bad B cmdline: $b_cmdline"; status=b_cmdline_bad; break ;;
			esac
			log "B cmdline: $b_cmdline"
			vmssh "$KEXEC_LOAD_TIMEOUT" "kexec -s -l /root/bzImage --command-line='$b_cmdline'" >>"$HLOG" 2>&1 ||
				{ status=kexec_load_failed; break; }
		else
			vmssh "$KEXEC_LOAD_TIMEOUT" "kexec -s -l /root/bzImage --reuse-cmdline --append='kho_reserve_mode=$VARIANT'" >>"$HLOG" 2>&1 ||
				{ status=kexec_load_failed; break; }
		fi
		vmssh 15 "(sleep 1; kexec -e) >/dev/null 2>&1 </dev/null &" >>"$HLOG" 2>&1
		log "kexec -e issued; waiting for kernel B (timeout ${B_TIMEOUT}s)"
		t0=$SECONDS
		wait_ssh "$B_TIMEOUT" "test \"\$(cat /proc/sys/kernel/random/boot_id)\" != $a_boot_id"
		local brc=$?
		if [ $brc -ne 0 ]; then
			[ $brc = 2 ] && status=qemu_exited_before_b || status=b_ssh_timeout
			break
		fi
		b_ssh_s=$((SECONDS - t0))
		log "B ssh up after ${b_ssh_s}s"
		vmssh 30 'cat /proc/cmdline; grep -E "MemTotal|MemFree|Hugetlb" /proc/meminfo; echo nr_1G=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages); cat /sys/kernel/debug/kho_bench/status; dmesg | grep -E "KHO_BENCH|KHO:|test_kho|WARNING|BUG:|HugeTLB|Run /sbin/init"' 2>&1 | tee -a "$HLOG"
		b_dmesg_init=$(vmssh 15 "dmesg | grep 'Run /sbin/init as init process' | tail -1")
		log "B dmesg init: $b_dmesg_init"
		b_hugetlb=$(vmssh 15 "awk '/^Hugetlb:/{print \$2}' /proc/meminfo")
		b_memfree=$(vmssh 15 "awk '/^MemFree:/{print \$2}' /proc/meminfo")
		if [ $VERIFY = 1 ]; then
			log "verify (timeout ${VERIFY_TIMEOUT}s)"
			vmssh "$VERIFY_TIMEOUT" 'echo 1 > /sys/kernel/debug/kho_bench/verify' >>"$HLOG" 2>&1
			local vline
			vline=$(vmssh 30 'dmesg | grep "kho_bench: VERIFY" | tail -1')
			log "verify: $vline"
			case "$vline" in
			*"VERIFY PASS"*) verify=PASS ;;
			*"VERIFY FAIL"*) verify=FAIL ;;
			*) verify=NONE; status="${status:+$status;}verify_no_result" ;;
			esac
		fi
		break
	done

	stop_vm || status="${status:+$status;}qemu_not_stopped"
	log "run $k finished: status=${status:-ok}"
	if [ $NOKEXEC = 1 ]; then
		python3 "$SCRIPT_DIR/kho_parse.py" show "$LOG" >>"$HLOG"
		[ "$status" = ok_no_kexec ]; return
	fi
	local row
	row=$(python3 "$SCRIPT_DIR/kho_parse.py" row --csv "$CSV" --log "$LOG" \
		--variant "$VARIANT" --pattern "$PATTERN" --count "$COUNT" --run "$k" \
		--status "$status" --verify "$verify" --params "$(echo "${PARAMS[*]} $EXTRA" | xargs)" \
		--b-hugetlb-kb "$b_hugetlb" --b-memfree-kb "$b_memfree" \
		--a-memfree-kb "$a_memfree" --git-desc "$GIT_DESC" \
		--b-dmesg-init "$b_dmesg_init" --b-ssh-s "$b_ssh_s" \
		--b-timeout "$B_TIMEOUT" --bzimage-sha "$LOCAL_SHA" \
		--b-cmdline-mode "$B_CMDLINE")
	log "csv: $row"
	[ "$(echo "$row" | cut -d, -f10)" = ok ]
}

fails=0
for i in $(seq 1 "$RUNS"); do
	if ! one_run; then
		fails=$((fails + 1))
		[ $STOP_ON_FAIL = 1 ] && break
	fi
done
echo "done: $RUNS run(s) requested, $fails failed; csv=$CSV"
[ $fails = 0 ]
