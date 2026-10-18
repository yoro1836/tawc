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
#                              [--cpu LIST] [--workloads NAME,NAME,...]
# --cpu pins the whole run with `taskset -c LIST` (e.g. 9 = prime core);
# unpinned runs on a phone swing 2x with core migration.
# Set TAWC_INSTALL_ID=<id> when more than one install exists.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

RUNS=7
OUT=""
CPU=""
WORKLOAD_FILTER=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs) RUNS="${2:?}"; shift 2 ;;
        --out) OUT="${2:?}"; shift 2 ;;
        --cpu) CPU="${2:?}"; shift 2 ;;
        --workloads)
            WORKLOAD_FILTER="${2-}"
            [[ $# -ge 2 && -n "$WORKLOAD_FILTER" ]] || { echo "ERROR: empty workload list" >&2; exit 2; }
            shift 2 ;;
        -h|--help) sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'; exit 0 ;;
        *) echo "ERROR: unknown arg: $1" >&2; exit 2 ;;
    esac
done
[[ "$RUNS" =~ ^[1-9][0-9]{0,6}$ ]] && (( RUNS <= 1000000 )) || { echo "ERROR: runs must be a positive integer up to 1000000" >&2; exit 2; }
if [[ -n "$CPU" ]]; then
    [[ "$CPU" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]] || { echo "ERROR: invalid CPU list" >&2; exit 2; }
    IFS=, read -r -a cpu_ranges <<< "$CPU"
    for range in "${cpu_ranges[@]}"; do
        first=${range%-*}; last=${range#*-}
        [[ ${#first} -le 4 && ${#last} -le 4 ]] && (( 10#$first <= 10#$last && 10#$last <= 1023 )) || {
            echo "ERROR: CPU ranges must be ascending and within 0..1023" >&2; exit 2;
        }
    done
fi

# name|shell snippet. Output is discarded; only wall time counts.
# Everything used ships in the base image.
WORKLOADS='spawn_true_x100|for ((i = 0; i < 100; i++)); do /usr/bin/true; done
find_usr|find /usr -type f | wc -l
ls_lR_usr_lib|ls -lR /usr/lib | wc -l
stat_many|find /usr/lib -type f -exec stat -c %s {} + | wc -l
grep_tree|{ grep -rl nonexistent-needle /usr/share /etc 2>/dev/null || [ "$?" -eq 1 ]; } | wc -l
tar_usr_share|tar cf - /usr/share | wc -c
pacman_Ql|pacman -Ql | wc -l
pacman_Qk|output=$(LC_ALL=C pacman -Qk 2>&1) || { status=$?; [ "$status" -eq 1 ] || exit "$status"; printf "%s\n" "$output" | awk "/^[^:]+: [0-9]+ total files, [0-9]+ missing files$/ { if (\$5 + 0 > 0) missing = 1; next } /^warning: [^:]+: .* [(]No such file or directory[)]$/ { next } NF { bad = 1 } END { exit (bad || !missing) }" || exit 1; }; printf "%s\n" "$output" | tail -1
bash_startup_x50|for ((i = 0; i < 50; i++)); do bash -c "exit 0"; done
sed_awk_pipe|seq 1 200000 | sed "s/1/x/" | awk "{n+=length(\$0)} END {print n}"'

select_workloads() {
    local requested=$1 wanted line match selected="" seen=","
    [[ "$requested" =~ ^[A-Za-z0-9_]+(,[A-Za-z0-9_]+)*$ ]] || { echo "ERROR: invalid workload list" >&2; return 2; }
    local -a names
    IFS=, read -r -a names <<< "$requested"
    for wanted in "${names[@]}"; do
        [[ "$seen" != *",$wanted,"* ]] || { echo "ERROR: duplicate workload: $wanted" >&2; return 2; }
        match=""
        while IFS= read -r line; do
            if [[ "${line%%|*}" == "$wanted" ]]; then match=$line; break; fi
        done <<< "$WORKLOADS"
        [[ -n "$match" ]] || { echo "ERROR: unknown workload: $wanted" >&2; return 2; }
        selected+="${selected:+$'\n'}$match"
        seen+="$wanted,"
    done
    printf '%s\n' "$selected"
}
if [[ -n "$WORKLOAD_FILTER" ]]; then WORKLOADS=$(select_workloads "$WORKLOAD_FILTER"); fi

# Runs inside the guest: time each workload RUNS times, print medians.
GUEST_FN='now() { echo "${EPOCHREALTIME/./}"; }   # microseconds
run_all() {
    local runs=$1 name cmd i t0 t1 ts sorted
    while IFS="|" read -r name cmd; do
        [ -n "$name" ] || continue
        ts=()
        # One warm-up run, not recorded: page cache and dentry state.
        if ! bash -e -o pipefail -c "$cmd" >/dev/null 2>&1 </dev/null; then
            printf "ERROR: macro %s warmup failed\n" "$name" >&2; return 1
        fi
        for ((i = 0; i < runs; i++)); do
            t0=$(now)
            if ! bash -e -o pipefail -c "$cmd" >/dev/null 2>&1 </dev/null; then
                printf "ERROR: macro %s measurement failed\n" "$name" >&2; return 1
            fi
            t1=$(now)
            ts+=($(( (t1 - t0) / 1000 )))
        done
        sorted=($(printf "%s\n" "${ts[@]}" | sort -n))
        printf "MACRO\t%s\t%s\t%s\t%s\n" "$name" "${sorted[$((runs / 2))]}" \
            "${sorted[0]}" "${sorted[$((runs - 1))]}"
    done
}'

# The broker re-joins argv into one shell string, so multi-line scripts
# and quoting do not survive it. Ship the script as base64 on one line.
SCRIPT=$(printf '%s\n' 'set -euo pipefail' "$GUEST_FN"
         printf 'WORKLOADS=%q\n' "$WORKLOADS"
         printf '%s\n' 'printf "%s\n" "$WORKLOADS" | run_all "$1"')
B64=$(printf '%s' "$SCRIPT" | base64 -w0)

# shellcheck disable=SC1091
. "$REPO_DIR/scripts/lib/select-device.sh"
result=$("$REPO_DIR/scripts/rootfs-run.sh" \
    "echo $B64 | base64 -d > /tmp/tawc-macro.sh && ${CPU:+taskset -c $CPU }bash /tmp/tawc-macro.sh $RUNS")

validate_results() {
    local expected
    expected=$(printf '%s\n' "$WORKLOADS" | cut -d '|' -f 1)
    awk -F'\t' -v expected="$expected" '
        BEGIN { count = split(expected, names, "\n") }
        $1 == "MACRO" {
            seen++
            if (NF != 5 || $2 != names[seen] || $3 !~ /^[0-9]+$/ ||
                $4 !~ /^[0-9]+$/ || $5 !~ /^[0-9]+$/ || $4 + 0 > $3 + 0 || $3 + 0 > $5 + 0) {
                print "ERROR: invalid or out-of-order macro result" >"/dev/stderr"; failed = 1
            }
            print $2 "\t" $3 "\t" $4 "\t" $5
        }
        END {
            if (seen != count) { print "ERROR: missing or duplicate macro results" >"/dev/stderr"; failed = 1 }
            exit failed
        }
    '
}
validated=$(printf '%s\n' "${result//$'\r'/}" | validate_results)
printf 'workload\tmedian_ms\tmin_ms\tmax_ms\n'
printf '%s\n' "$validated" | tee "${OUT:-/dev/null}"
