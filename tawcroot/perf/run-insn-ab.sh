#!/usr/bin/env bash
# User-mode instruction counts of real workloads under two runtimes.
#
# Wall time on a phone in use or dozing swings by 2-3x between rounds;
# `simpleperf stat -e instructions:u` repeats to ~0.01%, so it resolves
# small user-space changes that run-macro-shell.sh cannot. It counts
# guest and tawcroot user mode together and none of the kernel work, so
# use it for handler CPU changes, not for syscall-count or kernel-path
# changes. Runs from the adb shell domain against run-macro-shell.sh's
# rootfs; the app is never started.
#
# Usage: TAWC_TARGET=physical tawcroot/perf/run-insn-ab.sh BASE.so CAND.so
#            [--rounds 2] [--workload 'cmd'...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BASE="${1:?BASE.so}"; CAND="${2:?CAND.so}"; shift 2
ROUNDS=2
WORKLOADS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rounds) ROUNDS="${2:?}"; shift 2 ;;
        --workload) WORKLOADS+=("${2:?}"); shift 2 ;;
        *) echo "ERROR: unknown arg: $1" >&2; exit 2 ;;
    esac
done
[[ ${#WORKLOADS[@]} -gt 0 ]] || WORKLOADS=(
    'find /usr -type f'
    'ls -lR /usr/lib'
    'find /usr/lib -type f -exec stat -c %s {} +'
    'grep -rl nonexistent-needle /usr/share /etc'
    'tar cf - /usr/share'
)

# shellcheck source=../../scripts/lib/select-device.sh
source "$REPO_DIR/scripts/lib/select-device.sh"
# shellcheck source=../../scripts/lib/tawc-scratch.sh
source "$REPO_DIR/scripts/lib/tawc-scratch.sh"
DEV="$TAWC_SCRATCH/macro"
ROOT="$DEV/rootfs"
[[ "$(adb shell "[ -f $ROOT/.tawc-ready ] && echo yes" | tr -d '\r')" == yes ]] ||
    { echo "ERROR: no rootfs; run tawcroot/perf/run-macro-shell.sh first" >&2; exit 2; }
adb push -q "$BASE" "$DEV/insn-base"
adb push -q "$CAND" "$DEV/insn-cand"
adb shell "chmod 755 $DEV/insn-base $DEV/insn-cand"

declare -A sum
for ((r = 1; r <= ROUNDS; r++)); do
    for b in base cand; do
        for w in "${WORKLOADS[@]}"; do
            n=$(adb shell "cd $ROOT && simpleperf stat -e instructions:u $DEV/insn-$b -r $ROOT \
                    -b /dev:/dev -b /proc:/proc -b /sys:/sys -- /usr/bin/env -i \
                    PATH=/usr/bin:/bin LANG=C /usr/bin/bash -c '$w >/dev/null 2>&1' 2>&1" |
                tr -d '\r,' | awk '/instructions/ { print $1; exit }')
            sum["$b|$w"]=$(( ${sum["$b|$w"]:-0} + n ))
        done
    done
done
adb shell "rm -f $DEV/insn-base $DEV/insn-cand"

printf '%-48s %14s %14s %8s\n' workload base cand change
for w in "${WORKLOADS[@]}"; do
    b=$(( ${sum["base|$w"]} / ROUNDS )); c=$(( ${sum["cand|$w"]} / ROUNDS ))
    printf '%-48s %14d %14d %+7.2f%%\n' "$w" "$b" "$c" \
        "$(awk -v b="$b" -v c="$c" 'BEGIN { print (c - b) * 100 / b }')"
done
