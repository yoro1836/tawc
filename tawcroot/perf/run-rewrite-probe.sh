#!/usr/bin/env bash
# Controlled ARM64 rewriting probe: native shell and installed app runtime.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ITERATIONS=200000
ROUNDS=7
CPU=9
OUT=""
CC_BIN="${CC:-aarch64-linux-gnu-gcc}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iterations) ITERATIONS="${2:?}"; shift 2 ;;
        --rounds) ROUNDS="${2:?}"; shift 2 ;;
        --cpu) CPU="${2:?}"; shift 2 ;;
        --out) OUT="${2:?}"; shift 2 ;;
        --cc) CC_BIN="${2:?}"; shift 2 ;;
        --help|-h)
            echo "usage: $0 [--iterations N] [--rounds ODD] [--cpu N] [--out LOG] [--cc CC]"
            exit 0 ;;
        *) echo "ERROR: unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ "$ITERATIONS" =~ ^[1-9][0-9]{0,8}$ ]] && (( ITERATIONS <= 100000000 )) || { echo "ERROR: invalid iterations" >&2; exit 2; }
[[ "$ROUNDS" =~ ^[1-9][0-9]?$ ]] && (( ROUNDS <= 31 && ROUNDS % 2 )) || { echo "ERROR: rounds must be odd, within 1..31" >&2; exit 2; }
[[ "$CPU" =~ ^[0-9]{1,2}$ ]] && (( 10#$CPU <= 63 )) || { echo "ERROR: CPU must be within 0..63" >&2; exit 2; }
CPU=$((10#$CPU))
MASK=$(printf '%x' "$((1 << CPU))")
source "$REPO_DIR/scripts/lib/select-device.sh"
[[ "$(adb shell getprop ro.product.cpu.abi </dev/null | tr -d '\r')" == arm64-v8a ]] || { echo "ERROR: ARM64 device required" >&2; exit 2; }
source "$REPO_DIR/scripts/lib/tawc-install-id.sh"
source "$REPO_DIR/scripts/lib/tawc-scratch.sh"
WORK="$REPO_DIR/build/tawcroot-perf"
mkdir -p "$WORK"
[[ -n "$OUT" ]] || OUT="$WORK/rewrite-probe-$(date +%Y%m%d-%H%M%S).log"
BIN="$WORK/rewrite-probe-aarch64"
"$CC_BIN" -O2 -Wall -Wextra -Werror -static -pthread \
    -o "$BIN" "$SCRIPT_DIR/rewrite-probe.c" "$SCRIPT_DIR/rewrite-probe.S"
DEV="$TAWC_SCRATCH/rewrite-probe-$$"
GUEST="/tmp/tawc-rewrite-probe-$$"
CREATED=0
GUEST_CREATED=0
export TAWC_OP_TITLE=
cleanup() {
    if (( GUEST_CREATED )); then
        "$REPO_DIR/scripts/rootfs-run.sh" "rm -rf '$GUEST'" </dev/null >/dev/null || true
    fi
    if (( CREATED )); then adb shell "rm -rf '$DEV'" </dev/null >/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
adb shell "mkdir '$DEV'" </dev/null
CREATED=1
adb push -q "$BIN" "$DEV/probe" >/dev/null
adb shell "chmod 0755 '$DEV/probe'" </dev/null
"$REPO_DIR/scripts/rootfs-run.sh" "mkdir '$GUEST'" </dev/null
GUEST_CREATED=1
"$REPO_DIR/scripts/rootfs-run.sh" "cat > '$GUEST/probe' && chmod 0755 '$GUEST/probe'" <"$BIN"
{
    printf '# serial=%s install=%s cpu=%s iterations=%s rounds=%s\n' "$ANDROID_SERIAL" "$TAWC_INSTALL_ID" "$CPU" "$ITERATIONS" "$ROUNDS"
    printf '# hook=fixed_return real_tawcroot_dispatch=none\n'
    sha256sum "$BIN"
    info=$("$REPO_DIR/scripts/tawc-exec.sh" --action app-info </dev/null)
    printf '%s\n' "$info"
    library_dir=${info#nativeLibraryDir=}
    [[ "$library_dir" =~ ^/data/app/[A-Za-z0-9/_.=~+-]+/lib/arm64$ ]] || { echo "ERROR: unexpected app library path" >&2; exit 1; }
    "$REPO_DIR/scripts/tawc-exec.sh" -- /system/bin/sha256sum "$library_dir/libtawcroot.so" </dev/null
    printf '# native_context='
    adb shell id -Z </dev/null | tr -d '\r'
    printf '# app_context='
    "$REPO_DIR/scripts/tawc-exec.sh" -- /system/bin/cat /proc/self/attr/current </dev/null | tr -d '\000'
    printf '\n'
} >"$OUT"
run_backend() {
    local backend=$1 code=0
    printf 'BACKEND %s\n' "$backend" | tee -a "$OUT"
    if [[ "$backend" == native ]]; then
        adb shell "taskset '$MASK' '$DEV/probe' '$ITERATIONS' '$ROUNDS'" </dev/null \
            | tr -d '\r' | tee -a "$OUT" || code=$?
    else
        "$REPO_DIR/scripts/rootfs-run.sh" "taskset -c '$CPU' '$GUEST/probe' '$ITERATIONS' '$ROUNDS'" </dev/null \
            | tee -a "$OUT" || code=$?
    fi
    printf 'BACKEND_EXIT %s %s\n' "$backend" "$code" | tee -a "$OUT"
    if (( code != 0 && code != 77 )); then return "$code"; fi
    return "$code"
}
native_code=0
app_code=0
run_backend native || native_code=$?
run_backend app || app_code=$?
printf 'wrote %s\n' "$OUT"
if (( native_code != 0 && native_code != 77 )); then exit "$native_code"; fi
exit "$app_code"
