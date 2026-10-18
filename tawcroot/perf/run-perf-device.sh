#!/usr/bin/env bash
# On-device variant of run-perf.sh: native vs tawcroot on an arm64
# Android device, run from the adb `shell` domain against a minimal
# rootfs in the scratch dir. Measures the syscall layer only — the app's
# untrusted_app seccomp/SELinux policy is not in play here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

MODES="native,tawcroot"
ITERATIONS=20000
FILES=256
ROUNDS=10
OUT=""
TAWCROOT_BIN="$REPO_DIR/build/tawcroot-aarch64/libtawcroot.so"
BENCH_BIN=""
CC_BIN="${CC:-aarch64-linux-gnu-gcc}"
CPU_MASK=""

usage() {
    cat <<'EOF'
usage: tawcroot/perf/run-perf-device.sh [options]

Runs in TAWC_TARGET's device (or ANDROID_SERIAL). Rounds interleave
backends.

Options:
  --modes LIST      Comma list: native,tawcroot (default: both)
  --tawcroot PATH   arm64 tawcroot (default: build/tawcroot-aarch64/libtawcroot.so)
  --bench-bin PATH  prebuilt static arm64 tawcroot-perf
  --cc PATH         static-capable arm64 C compiler (default: aarch64-linux-gnu-gcc)
  --iterations N    Inner-loop iterations (default: 20000)
  --files N         Files in the work dir (default: 256)
  --rounds N        Rounds per backend (default: 10)
  --out PATH        TSV output (default: build/tawcroot-perf/device-*.tsv)
  --cpu MASK        Pin runs with taskset (hex mask, e.g. 200 = cpu9)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --modes) MODES="${2:?}"; shift 2 ;;
        --tawcroot) TAWCROOT_BIN="${2:?}"; shift 2 ;;
        --bench-bin) BENCH_BIN="${2:?}"; shift 2 ;;
        --cc) CC_BIN="${2:?}"; shift 2 ;;
        --iterations) ITERATIONS="${2:?}"; shift 2 ;;
        --files) FILES="${2:?}"; shift 2 ;;
        --rounds) ROUNDS="${2:?}"; shift 2 ;;
        --out) OUT="${2:?}"; shift 2 ;;
        --cpu) CPU_MASK="${2:?}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# shellcheck source=../../scripts/lib/select-device.sh
source "$REPO_DIR/scripts/lib/select-device.sh"
# shellcheck source=../../scripts/lib/tawc-scratch.sh
source "$REPO_DIR/scripts/lib/tawc-scratch.sh"

WORK_ROOT="$REPO_DIR/build/tawcroot-perf"
mkdir -p "$WORK_ROOT"
[[ -n "$OUT" ]] || OUT="$WORK_ROOT/device-$(date +%Y%m%d-%H%M%S).tsv"

if [[ -z "$BENCH_BIN" ]]; then
    BENCH_BIN="$WORK_ROOT/tawcroot-perf-aarch64"
    "$CC_BIN" -O2 -static -Wall -Wextra -o "$BENCH_BIN" "$SCRIPT_DIR/tawcroot-perf.c"
fi
[[ -x "$TAWCROOT_BIN" ]] || { echo "ERROR: missing $TAWCROOT_BIN (tawcroot/build.sh --abi=aarch64)" >&2; exit 2; }

DEV="$TAWC_SCRATCH/perf"
ROOTFS="$DEV/rootfs"
adb shell "rm -rf $ROOTFS && mkdir -p $ROOTFS/tmp/tawcroot-perf && chmod 1777 $ROOTFS/tmp" >/dev/null
adb push -q "$TAWCROOT_BIN" "$DEV/tawcroot" >/dev/null
adb push -q "$BENCH_BIN" "$ROOTFS/tmp/tawcroot-perf/tawcroot-perf" >/dev/null
adb shell "chmod 0755 $DEV/tawcroot $ROOTFS/tmp/tawcroot-perf/tawcroot-perf"

BENCH_ARGS="--iterations $ITERATIONS --files $FILES --setup"
PIN=""
[[ -n "$CPU_MASK" ]] && PIN="taskset $CPU_MASK "
cat >"$WORK_ROOT/device-run.sh" <<EOF
R=$ROOTFS
B=/tmp/tawcroot-perf/tawcroot-perf
round=1
while [ \$round -le $ROUNDS ]; do
  for m in ${MODES//,/ }; do
    echo "ROUND \$m \$round"
    case \$m in
      native) $PIN\$R\$B --root-prefix \$R --self \$R\$B $BENCH_ARGS ;;
      tawcroot) (cd \$R && $PIN$DEV/tawcroot -r \$R -- \$B --self \$B $BENCH_ARGS) ;;
    esac || echo "FAIL \$m \$round \$?"
  done
  round=\$((round + 1))
done
EOF
adb push -q "$WORK_ROOT/device-run.sh" "$DEV/run.sh" >/dev/null

{
    printf '# device=%s kernel=%s\n' "$(adb shell getprop ro.product.model | tr -d '\r')" "$(adb shell uname -r | tr -d '\r')"
    printf '# iterations=%s files=%s rounds=%s cpu=%s\n' "$ITERATIONS" "$FILES" "$ROUNDS" "${CPU_MASK:-any}"
    printf 'backend\tround\tbenchmark\telapsed_ns\tops\tns_per_op\n'
} >"$OUT"

adb shell "sh $DEV/run.sh" 2>&1 | tr -d '\r' | awk -v out="$OUT" '
    $1 == "ROUND" { backend = $2; round = $3; print "-- " backend " round " round; next }
    $1 == "RESULT" { print backend "\t" round "\t" $2 "\t" $3 "\t" $4 "\t" $5 >>out; next }
    $1 == "FAIL" { print "ERROR: " $0; failed = 1; next }
    { print }
    END { exit failed }
'
adb shell "rm -rf $DEV"

echo "wrote $OUT"
awk -f "$SCRIPT_DIR/summarize.awk" "$OUT"
