#!/usr/bin/env bash
# Real-workload timings inside an installed distro, through the dev exec
# broker (app domain, the same path users run). Complements the syscall
# microbenchmarks: these are the workloads native-ish speed is judged on.
#
# The app domain has no native baseline (it cannot execve rootfs
# binaries outside tawcroot), so results are build-vs-build: install
# each build into the dev app and rerun.
#
# Usage: TAWC_TARGET=physical tawcroot/perf/macro.sh [--runs N] [--out FILE]
#                                                    [--cpu LIST]
# --cpu pins the whole run with `taskset -c LIST` (e.g. 9 = prime core);
# unpinned runs on a phone swing 2x with core migration.
# Set TAWC_INSTALL_ID=<id> when more than one install exists.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

RUNS=7
OUT=""
CPU=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs) RUNS="${2:?}"; shift 2 ;;
        --out) OUT="${2:?}"; shift 2 ;;
        --cpu) CPU="${2:?}"; shift 2 ;;
        -h|--help) sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'; exit 0 ;;
        *) echo "ERROR: unknown arg: $1" >&2; exit 2 ;;
    esac
done

# name|shell snippet. Output is discarded; only wall time counts.
# Everything used ships in the base image.
WORKLOADS='spawn_true_x100|for i in $(seq 100); do /usr/bin/true; done
find_usr|find /usr -type f | wc -l
ls_lR_usr_lib|ls -lR /usr/lib | wc -l
stat_many|find /usr/lib -type f -exec stat -c %s {} + | wc -l
grep_tree|grep -rl nonexistent-needle /usr/share /etc 2>/dev/null | wc -l
tar_usr_share|tar cf - /usr/share | wc -c
pacman_Ql|pacman -Ql | wc -l
pacman_Qk|pacman -Qk 2>&1 | tail -1
bash_startup_x50|for i in $(seq 50); do bash -c "exit 0"; done
sed_awk_pipe|seq 1 200000 | sed "s/1/x/" | awk "{n+=length(\$0)} END {print n}"'

# Runs inside the guest: time each workload RUNS times, print medians.
GUEST_FN='now() { echo "${EPOCHREALTIME/./}"; }   # microseconds
run_all() {
    local runs=$1 name cmd i t0 t1 ts sorted
    while IFS="|" read -r name cmd; do
        [ -n "$name" ] || continue
        ts=()
        # One warm-up run, not recorded: page cache and dentry state.
        bash -c "$cmd" >/dev/null 2>&1 </dev/null || true
        for ((i = 0; i < runs; i++)); do
            t0=$(now); bash -c "$cmd" >/dev/null 2>&1 </dev/null || true; t1=$(now)
            ts+=($(( (t1 - t0) / 1000 )))
        done
        sorted=($(printf "%s\n" "${ts[@]}" | sort -n))
        printf "MACRO\t%s\t%s\t%s\t%s\n" "$name" "${sorted[$((runs / 2))]}" \
            "${sorted[0]}" "${sorted[$((runs - 1))]}"
    done
}'

# The broker re-joins argv into one shell string, so multi-line scripts
# and quoting do not survive it. Ship the script as base64 on one line.
SCRIPT=$(printf '%s\n' "$GUEST_FN"
         printf 'WORKLOADS=%q\n' "$WORKLOADS"
         printf '%s\n' 'printf "%s\n" "$WORKLOADS" | run_all "$1"')
B64=$(printf '%s' "$SCRIPT" | base64 -w0)

# shellcheck disable=SC1091
. "$REPO_DIR/scripts/lib/select-device.sh"
result=$("$REPO_DIR/scripts/rootfs-run.sh" \
    "echo $B64 | base64 -d > /tmp/tawc-macro.sh && ${CPU:+taskset -c $CPU }bash /tmp/tawc-macro.sh $RUNS")

printf 'workload\tmedian_ms\tmin_ms\tmax_ms\n'
printf '%s\n' "$result" | awk -F'\t' '$1 == "MACRO" { print $2 "\t" $3 "\t" $4 "\t" $5 }' \
    | tee "${OUT:-/dev/null}"
