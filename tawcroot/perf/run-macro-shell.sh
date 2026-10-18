#!/usr/bin/env bash
# Real-workload A/B of two tawcroot runtimes WITHOUT the app.
#
# macro.sh goes through the app's exec broker, which (re)launches the
# app's activity — and an APK swap per round kills and relaunches it.
# This runner instead extracts a distro tarball into the device scratch
# dir and runs tawcroot from the adb `shell` domain, so nothing appears
# on the phone's screen and swapping runtimes is a binary push. Shell
# domain is not the app's SELinux/seccomp context; use it for
# runtime-vs-runtime comparisons, not app-domain absolutes.
#
# Usage: TAWC_TARGET=physical tawcroot/perf/run-macro-shell.sh \
#            --rootfs-tar ArchLinuxARM-aarch64-latest.tar.gz \
#            --tawcroot BASE.so --compare CANDIDATE.so \
#            [--rounds 3] [--runs 7] [--cpu 9] [--workloads a,b] [--cleanup]
# The extracted rootfs stays on the device for later runs until
# --cleanup (which removes it and exits).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

TAR="" BASE="" CAND="" ROUNDS=3 RUNS=7 CPU=9 FILTER="" CLEANUP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rootfs-tar) TAR="${2:?}"; shift 2 ;;
        --tawcroot) BASE="${2:?}"; shift 2 ;;
        --compare) CAND="${2:?}"; shift 2 ;;
        --rounds) ROUNDS="${2:?}"; shift 2 ;;
        --runs) RUNS="${2:?}"; shift 2 ;;
        --cpu) CPU="${2:?}"; shift 2 ;;
        --workloads) FILTER="${2:?}"; shift 2 ;;
        --cleanup) CLEANUP=1; shift ;;
        -h|--help) sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'; exit 0 ;;
        *) echo "ERROR: unknown arg: $1" >&2; exit 2 ;;
    esac
done

# shellcheck source=../../scripts/lib/select-device.sh
source "$REPO_DIR/scripts/lib/select-device.sh"
# shellcheck source=../../scripts/lib/tawc-scratch.sh
source "$REPO_DIR/scripts/lib/tawc-scratch.sh"
DEV="$TAWC_SCRATCH/macro"
ROOT="$DEV/rootfs"

if [[ "$CLEANUP" == 1 ]]; then
    adb shell "rm -rf $DEV"
    echo "removed $DEV"
    exit 0
fi
[[ -x "$BASE" && -x "$CAND" ]] || { echo "ERROR: --tawcroot and --compare must be executables" >&2; exit 2; }

if [[ "$(adb shell "[ -f $ROOT/.tawc-ready ] && echo yes" | tr -d '\r')" != yes ]]; then
    [[ -f "$TAR" ]] || { echo "ERROR: rootfs not on device; pass --rootfs-tar" >&2; exit 2; }
    echo "pushing $(basename "$TAR") ..."
    adb shell "rm -rf $DEV && mkdir -p $ROOT"
    adb push -q "$TAR" "$DEV/rootfs.tar.gz"
    echo "extracting on device ..."
    # Device nodes and ownership changes fail as the shell user; the
    # workloads only need the regular tree.
    adb shell "cd $ROOT && tar xzf ../rootfs.tar.gz 2>/dev/null; rm -f ../rootfs.tar.gz; \
               mkdir -p tmp dev proc sys && chmod 1777 tmp && touch .tawc-ready"
fi
adb push -q "$BASE" "$DEV/tawcroot-base"
adb push -q "$CAND" "$DEV/tawcroot-cand"
adb shell "chmod 755 $DEV/tawcroot-base $DEV/tawcroot-cand"

# Same workload set and timing loop as macro.sh.
WORKLOADS='find_usr|find /usr -type f | wc -l
ls_lR_usr_lib|ls -lR /usr/lib | wc -l
stat_many|find /usr/lib -type f -exec stat -c %s {} + | wc -l
grep_tree|grep -rl nonexistent-needle /usr/share /etc 2>/dev/null | wc -l
tar_usr_share|tar cf - /usr/share | wc -c
pacman_Ql|pacman -Ql | wc -l
pacman_Qk|pacman -Qk 2>&1 | tail -1
spawn_true_x100|for ((i = 0; i < 100; i++)); do /usr/bin/true; done
bash_startup_x50|for ((i = 0; i < 50; i++)); do bash -c "exit 0"; done'
if [[ -n "$FILTER" ]]; then
    WORKLOADS=$(printf '%s\n' "$WORKLOADS" | awk -F'|' -v want=",$FILTER," 'index(want, "," $1 ",")')
fi
GUEST=$(cat <<'EOF'
now() { echo "${EPOCHREALTIME/./}"; }
runs=$1
while IFS="|" read -r name cmd; do
    [ -n "$name" ] || continue
    bash -c "$cmd" >/dev/null 2>&1 </dev/null || true
    ts=()
    for ((i = 0; i < runs; i++)); do
        t0=$(now); bash -c "$cmd" >/dev/null 2>&1 </dev/null || true; t1=$(now)
        ts+=($(( (t1 - t0) / 1000 )))
    done
    sorted=($(printf "%s\n" "${ts[@]}" | sort -n))
    printf "MACRO\t%s\t%s\t%s\n" "$name" "${sorted[$((runs / 2))]}" "${sorted[0]}"
done < /tmp/tawc-macro-workloads
EOF
)
WORK_HOST="$REPO_DIR/build/tawcroot-perf"
mkdir -p "$WORK_HOST"
printf '%s\n' "$WORKLOADS" >"$WORK_HOST/macro-workloads"
printf '%s\n' "$GUEST" >"$WORK_HOST/macro-guest.sh"
adb push -q "$WORK_HOST/macro-workloads" "$ROOT/tmp/tawc-macro-workloads"
adb push -q "$WORK_HOST/macro-guest.sh" "$ROOT/tmp/tawc-macro.sh"

# toybox taskset takes a hex mask, not -c.
MASK=$(printf '%x' $((1 << CPU)))
OUT="$WORK_HOST/macro-shell-$(date +%Y%m%d-%H%M%S).tsv"
printf 'runtime\tround\tworkload\tmedian_ms\tmin_ms\n' >"$OUT"
for ((r = 1; r <= ROUNDS; r++)); do
    # Alternate which runtime goes first each round.
    order="base cand"; (( r % 2 == 0 )) && order="cand base"
    for b in $order; do
        echo "round $r: $b"
        adb shell "cd $ROOT && taskset $MASK $DEV/tawcroot-$b -r $ROOT \
                   -b /dev:/dev -b /proc:/proc -b /sys:/sys -- \
                   /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root LANG=C \
                   /usr/bin/bash /tmp/tawc-macro.sh $RUNS" | tr -d '\r' |
            awk -F'\t' -v b="$b" -v r="$r" '$1 == "MACRO" { print b "\t" r "\t" $2 "\t" $3 "\t" $4 }' >>"$OUT"
    done
done

echo "wrote $OUT"
python3 - "$OUT" <<'PY'
import sys, statistics, collections
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1])][1:]
d = collections.defaultdict(lambda: collections.defaultdict(list))
lo = collections.defaultdict(lambda: collections.defaultdict(list))
order = []
for b, r, w, ms, mn in rows:
    if w not in order: order.append(w)
    d[w][b].append(int(ms))
    lo[w][b].append(int(mn))
# min: fastest single run per side, robust to background load and
# frequency drops on a phone in use or dozing.
print(f"{'workload':18} {'base ms':>16} {'cand ms':>16} {'median':>7} {'min':>7}")
for w in order:
    b, c = d[w]['base'], d[w]['cand']
    if b and c:
        print(f"{w:18} {str(sorted(b)):>16} {str(sorted(c)):>16}"
              f" {statistics.median(c) / statistics.median(b):7.3f}"
              f" {min(lo[w]['cand']) / min(lo[w]['base']):7.3f}")
PY
