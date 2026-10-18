#!/usr/bin/env bash
# Interleaved native/tawcroot exec probes in the Android shell domain.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROUNDS=5
ITERATIONS=1000
CPU=9
TAWCROOT="$REPO_DIR/build/tawcroot-aarch64/libtawcroot.so"
COMPARE=""
OUT=""
CC_BIN="${CC:-aarch64-linux-gnu-gcc}"
usage() {
    echo "usage: $0 [--rounds N] [--iterations N] [--cpu LIST] [--tawcroot PATH] [--compare PATH] [--out TSV] [--cc CC]"
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rounds) ROUNDS="${2:?}"; shift 2 ;;
        --iterations) ITERATIONS="${2:?}"; shift 2 ;;
        --cpu) CPU="${2:?}"; shift 2 ;;
        --tawcroot) TAWCROOT="${2:?}"; shift 2 ;;
        --compare) COMPARE="${2:?}"; shift 2 ;;
        --out) OUT="${2:?}"; shift 2 ;;
        --cc) CC_BIN="${2:?}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
for n in "$ROUNDS" "$ITERATIONS"; do
    [[ "$n" =~ ^[1-9][0-9]{0,6}$ ]] && (( n <= 1000000 )) || { echo "ERROR: invalid count: $n" >&2; exit 2; }
done
[[ "$CPU" =~ ^[0-9]+([,-][0-9]+)*$ ]] || { echo "ERROR: invalid CPU list" >&2; exit 2; }
CPU_MASK=$(python3 - "$CPU" <<'PY'
import sys
mask = 0
for item in sys.argv[1].split(','):
    bounds = item.split('-')
    if len(bounds) > 2:
        sys.exit('ERROR: invalid CPU range')
    first, last = int(bounds[0]), int(bounds[-1])
    if first > last or last > 1023:
        sys.exit('ERROR: CPU ranges must be ascending and within 0..1023')
    for cpu in range(first, last + 1):
        mask |= 1 << cpu
print(f'{mask:x}')
PY
)
[[ -x "$TAWCROOT" ]] || { echo "ERROR: missing tawcroot: $TAWCROOT" >&2; exit 2; }
if [[ -n "$COMPARE" && ! -x "$COMPARE" ]]; then echo "ERROR: missing candidate: $COMPARE" >&2; exit 2; fi
ORDER_ODD='native tawcroot'
ORDER_EVEN='tawcroot native'
if [[ -n "$COMPARE" ]]; then ORDER_ODD='native baseline candidate'; ORDER_EVEN='candidate baseline native'; fi
# shellcheck source=../../scripts/lib/select-device.sh
source "$REPO_DIR/scripts/lib/select-device.sh"
ABI=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
[[ "$ABI" == arm64-v8a ]] || { echo "ERROR: Android arm64 required (got $ABI)" >&2; exit 2; }
WORK="$REPO_DIR/build/tawcroot-perf"
mkdir -p "$WORK"
[[ -n "$OUT" ]] || OUT="$WORK/exec-cost-$(date +%Y%m%d-%H%M%S).tsv"
PROBE="$WORK/exec-cost-probe-aarch64"
TINY="$WORK/exec-tiny-aarch64"
"$CC_BIN" -O2 -static -Wall -Wextra -Werror -o "$PROBE" "$SCRIPT_DIR/exec-cost.c"
"$CC_BIN" -O2 -static -nostdlib -fno-pie -no-pie -fno-stack-protector -Wl,-e,_start \
    -Wall -Wextra -Werror -o "$TINY" "$SCRIPT_DIR/exec-tiny.c"
# shellcheck source=../../scripts/lib/tawc-scratch.sh
source "$REPO_DIR/scripts/lib/tawc-scratch.sh"
DEV="$TAWC_SCRATCH/exec-cost-$$"
CREATED=0
RUN_SCRIPT=""
cleanup() {
    if (( CREATED )); then adb shell "rm -rf '$DEV'" >/dev/null || true; fi
    if [[ -n "$RUN_SCRIPT" ]]; then rm -f -- "$RUN_SCRIPT"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
adb shell "mkdir '$DEV'"
CREATED=1
ROOTFS="$DEV/rootfs"
adb shell "mkdir -p '$ROOTFS/bin' '$ROOTFS/tmp' '$ROOTFS/dev'"
adb push -q "$TAWCROOT" "$DEV/tawcroot" >/dev/null
if [[ -n "$COMPARE" ]]; then
    adb push -q "$COMPARE" "$DEV/candidate" >/dev/null
    adb shell "chmod 0755 '$DEV/candidate'"
fi
adb push -q "$PROBE" "$ROOTFS/bin/exec-cost" >/dev/null
adb push -q "$TINY" "$ROOTFS/bin/exec-tiny" >/dev/null
adb shell "chmod 0755 '$DEV/tawcroot' '$ROOTFS/bin/exec-cost' '$ROOTFS/bin/exec-tiny'"
RUN_SCRIPT="$WORK/exec-cost-run-$$.sh"
cat >"$RUN_SCRIPT" <<EOF
#!/system/bin/sh
set -eu
D='$DEV'
R='$ROOTFS'
cd "\$R"
round=1
while [ "\$round" -le $ROUNDS ]; do
    if [ \$((round % 2)) -eq 1 ]; then order='$ORDER_ODD'; else order='$ORDER_EVEN'; fi
    for backend in \$order; do
        if [ "\$backend" = native ]; then prefix="\$R"; else prefix=''; fi
        for phase in fork raw-fork spawn kernel faults; do
            echo "ROUND \$backend \$round \$phase"
            case "\$phase" in
                fork|raw-fork) set -- "\$phase" $ITERATIONS ;;
                spawn) set -- spawn $ITERATIONS "\$prefix/bin/exec-tiny" ;;
                kernel) set -- kernel $ITERATIONS "\$prefix/tmp/kernel-\$round-\$backend" ;;
                faults) set -- faults 2048 "\$prefix/tmp/faults-\$round-\$backend" ;;
            esac
            if [ "\$backend" = native ]; then
                set -- "\$R/bin/exec-cost" "\$@"
            else
                runtime="\$D/tawcroot"
                if [ "\$backend" = candidate ]; then runtime="\$D/candidate"; fi
                set -- "\$runtime" -r "\$R" -b /dev:/dev -- /bin/exec-cost "\$@"
            fi
            taskset '$CPU_MASK' "\$@"
        done
    done
    round=\$((round + 1))
done
EOF
adb push -q "$RUN_SCRIPT" "$DEV/run.sh" >/dev/null
{
    printf '# domain=shell serial=%s abi=%s cpu=%s cpu_mask=%s rounds=%s iterations=%s\n' "$ANDROID_SERIAL" "$ABI" "$CPU" "$CPU_MASK" "$ROUNDS" "$ITERATIONS"
    printf '# tawcroot=%s compiler=%s\n' "$TAWCROOT" "$CC_BIN"
    sha256sum "$TAWCROOT" | awk '{ print "# runtime_sha256=" $1 }'
    if [[ -n "$COMPARE" ]]; then
        printf '# baseline=%s candidate=%s\n' "$TAWCROOT" "$COMPARE"
        sha256sum "$TAWCROOT" | awk '{ print "# baseline_runtime_sha256=" $1 }'
        sha256sum "$COMPARE" | awk '{ print "# candidate_runtime_sha256=" $1 }'
    fi
    adb shell 'printf "# model="; getprop ro.product.model; printf "# kernel="; uname -r; printf "# page_size="; getconf PAGESIZE' | tr -d '\r'
    printf 'backend\tround\tphase\tkind\tname\tcount\tns_per_op\tself_minflt\tchild_minflt\tself_majflt\tchild_majflt\tself_user_us\tself_sys_us\tchild_user_us\tchild_sys_us\n'
} >"$OUT"
adb shell "sh '$DEV/run.sh'" | tr -d '\r' | awk -v out="$OUT" '
    $1 == "ROUND" { backend = $2; round = $3; phase = $4; print "-- " backend " round " round " " phase; next }
    /^#/ { print "# backend=" backend " round=" round " phase=" phase " " $0 >>out; next }
    $1 == "RESULT" { print backend "\t" round "\t" phase "\t" $0 >>out; next }
    { print "ERROR: unexpected probe output: " $0 >"/dev/stderr"; failed = 1 }
    END { exit failed }
'
python3 - "$OUT" "${COMPARE:+yes}" "$ROUNDS" <<'PY'
import csv
import statistics
import sys
from collections import defaultdict
rows = defaultdict(lambda: defaultdict(list))
compared, rounds = bool(sys.argv[2]), int(sys.argv[3])
backends = ['native', 'baseline', 'candidate'] if compared else ['native', 'tawcroot']
with open(sys.argv[1]) as stream:
    for row in csv.DictReader((line for line in stream if not line.startswith('#')), delimiter='\t'):
        rows[(row['phase'], row['name'])][row['backend']].append(float(row['ns_per_op']))
if not rows:
    sys.exit('ERROR: no probe results')
print('phase / operation                         native us  baseline us  candidate us  cand/base  rounds' if compared else
      'phase / operation                         native us  tawcroot us  ratio')
for (phase, name), values in rows.items():
    if set(values) != set(backends) or any(len(values[b]) != rounds for b in backends):
        sys.exit(f'ERROR: incomplete rounds for {phase}/{name}')
    medians = [statistics.median(values[b]) / 1000 for b in backends]
    if compared:
        native, baseline, candidate = medians
        print(f'{phase + " / " + name:<41} {native:9.2f} {baseline:12.2f} {candidate:13.2f} {candidate / baseline:10.2f} {rounds:7d}')
    else:
        native, guest = medians
        print(f'{phase + " / " + name:<41} {native:9.2f} {guest:12.2f} {guest / native:6.2f}')
print(f'wrote {sys.argv[1]}')
PY
