#!/bin/sh
# Boot a rootfs's systemd under tawcroot as an unprivileged user and
# check it comes up. Prints failed units, the console log and any
# denied syscalls (<store>/eacces.log).
#
#   tawcroot/systemd-smoke.sh TAWCROOT ROOTFS [SECS]
#
# Fails if PID 1 dies or the system isn't running/degraded after SECS.
set -u
T=$(realpath "$1")
R=$(realpath "$2")
SECS=${3:-40}
S=$(dirname "$R")/tawcroot
B="-r $R -b /proc:/proc -b /sys:/sys -b /dev:/dev"

mkdir -p "$S"
rm -f "$S/fault.log"
: > "$S/eacces.log"
: > "$S/console.log"
# shellcheck disable=SC2086
"$T" $B --console "$S/console.log" --init --cgroup -- \
    /usr/bin/env -i container=tawc /usr/lib/systemd/systemd > "$S/out.log" 2>&1 &
P=$!
sleep "$SECS"

state=dead
if kill -0 $P 2>/dev/null; then
    # shellcheck disable=SC2086
    state=$("$T" $B --join -- /usr/bin/systemctl is-system-running 2>&1)
    echo "== failed units"
    # shellcheck disable=SC2086
    "$T" $B --join -- /usr/bin/systemctl --no-pager --failed 2>&1
    kill -9 $P
else
    wait $P
    echo "== tawcroot exited early, status $?"
fi
echo "== tawcroot output"; cat "$S/out.log"
echo "== console log"; cat "$S/console.log"
echo "== fault.log"; cat "$S/fault.log" 2>/dev/null
echo "== denied syscalls (count nr errno path)"
cut -d" " -f2- "$S/eacces.log" | sort | uniq -c | sort -rn | head -n 60
echo "== state: $state"
case $state in running|degraded) exit 0 ;; *) exit 1 ;; esac
