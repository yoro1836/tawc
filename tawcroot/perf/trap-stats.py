#!/usr/bin/env python3
"""Attribute real-workload time to tawcroot overhead on a device.

Needs the dev app running a runtime built with -DTAWCROOT_STATS (see
include/stats.h; swap it in with repack-runtime-apk.sh). For each
workload it snapshots the shared `<distro>/tawcroot-stats` file, runs the
workload in the installed distro through the exec broker (pinned with
taskset), snapshots again, and prints the difference: wall time, trap
counts and handler time per syscall, guest execs with their prepare and
--exec-child init time. An `empty` workload measures the fixed cost of
one broker invocation, which is subtracted from the others.

With --shell BIN the app is not used: the stats-built runtime BIN runs
the workloads from the adb shell domain against the rootfs that
run-macro-shell.sh extracted into the device scratch dir.

Usage: TAWC_TARGET=physical tawcroot/perf/trap-stats.py [--cpu 9]
           [--install arch] [--top 12] [--workloads a,b,...] [--shell BIN]
"""
import argparse
import base64
import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NR_MAX = 1024
HEADER_WORDS = 56
MAGIC = 0x7461776373746135
PHASES = ["state", "sv:root", "sv:binds", "sv:store", "sv:memo", "sv:handler", "sv:rest", "binary", "ld.so", "entry"]

WORKLOADS = [
    ("empty", "true"),
    ("spawn_true_x100", "for ((i = 0; i < 100; i++)); do /usr/bin/true; done"),
    ("bash_startup_x50", 'for ((i = 0; i < 50; i++)); do bash -c "exit 0"; done'),
    ("find_usr", "find /usr -type f | wc -l"),
    ("ls_lR_usr_lib", "ls -lR /usr/lib | wc -l"),
    ("stat_many", "find /usr/lib -type f -exec stat -c %s {} + | wc -l"),
    ("grep_tree", "grep -rl nonexistent-needle /usr/share /etc | wc -l"),
    ("tar_usr_share", "tar cf - /usr/share | wc -c"),
    ("pacman_Ql", "pacman -Ql | wc -l"),
    ("pacman_Qk", "pacman -Qk 2>&1 | tail -1"),
    ("sed_awk_pipe", 'seq 1 200000 | sed "s/1/x/" | awk "{n+=length(\\$0)} END {print n}"'),
]


def syscall_names():
    names = {}
    path = "/usr/aarch64-linux-gnu/include/asm-generic/unistd.h"
    try:
        for line in open(path):
            m = re.match(r"#define __NR(?:3264)?_(\w+)\s+(\d+)\s*$", line)
            if m:
                names.setdefault(int(m.group(2)), m.group(1))
    except OSError:
        pass
    return names


SHELL_DEV = "/data/local/tmp/tawc-dev/macro"   # run-macro-shell.sh's dir
SHELL_ROOT = SHELL_DEV + "/rootfs"
SHELL_BIN = SHELL_DEV + "/tawcroot-stats-bin"


def adb_shell(cmd):
    return subprocess.run(["adb", "shell", cmd], capture_output=True, check=False)


def select_device():
    if os.environ.get("ANDROID_SERIAL"):
        return
    r = subprocess.run(["bash", "-c", f"source {REPO}/scripts/lib/select-device.sh"
                        " >/dev/null && echo $ANDROID_SERIAL"],
                       capture_output=True, text=True, check=False)
    serial = r.stdout.strip()
    if not serial:
        sys.exit(f"ERROR: no device: {r.stderr.strip()}")
    os.environ["ANDROID_SERIAL"] = serial


def broker(args, stdin=None):
    cmd = [os.path.join(REPO, "scripts/tawc-exec.sh")] + args
    env = dict(os.environ, TAWC_OP_TITLE="")
    return subprocess.run(cmd, input=stdin, capture_output=True, env=env, check=False)


def snapshot(path, shell):
    read = f"base64 -w0 {path} 2>/dev/null || base64 {path}"
    r = adb_shell(read) if shell else broker(["--", "/system/bin/sh", "-c", read])
    raw = base64.b64decode(r.stdout.strip() or b"")
    words = HEADER_WORDS + 4 * NR_MAX
    if len(raw) < words * 8:
        sys.exit(f"ERROR: stats file {path} missing or short ({len(raw)} bytes);"
                 " is a -DTAWCROOT_STATS runtime installed?")
    v = struct.unpack(f"<{words}Q", raw[: words * 8])
    if v[0] != MAGIC:
        sys.exit("ERROR: bad stats magic")
    return v


def diff(a, b):
    return [y - x for x, y in zip(a, b)]


def run_workload(install, cpu, cmd, shell):
    pin = f"taskset -c {cpu} " if cpu else ""
    # Time inside the guest so broker startup is excluded from wall.
    script = (f's=$EPOCHREALTIME; {pin}bash -c {quote(cmd)} >/dev/null 2>&1 </dev/null; '
              'e=$EPOCHREALTIME; echo WALL $(( ${e/./} - ${s/./} ))')
    if shell:
        r = adb_shell(f"cd {SHELL_ROOT} && {SHELL_BIN} -r {SHELL_ROOT}"
                      " -b /dev:/dev -b /proc:/proc -b /sys:/sys --"
                      " /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root LANG=C"
                      f" /usr/bin/bash -c {quote(script)}")
    else:
        r = broker(["--in-rootfs", install, "--", "bash", "-c", quote(script)])
    m = re.search(rb"WALL (\d+)", r.stdout)
    if not m:
        sys.exit(f"ERROR: workload failed: {cmd}\n{r.stdout.decode()}{r.stderr.decode()}")
    return int(m.group(1))


def quote(s):
    return "'" + s.replace("'", "'\"'\"'") + "'"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpu", default="9")
    ap.add_argument("--install", default="arch")
    ap.add_argument("--app-data", default="/data/user/0/me.phie.tawc.dev")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--workloads", default="")
    ap.add_argument("--shell", metavar="BIN", default="")
    opt = ap.parse_args()
    stats_path = f"{opt.app_data}/distros/{opt.install}/tawcroot-stats"
    shell = bool(opt.shell)
    if shell:
        select_device()
        if adb_shell(f"[ -f {SHELL_ROOT}/.tawc-ready ] && echo yes").stdout.strip() != b"yes":
            sys.exit("ERROR: no shell rootfs; run tawcroot/perf/run-macro-shell.sh first")
        subprocess.run(["adb", "push", "-q", opt.shell, SHELL_BIN], check=True)
        adb_shell(f"chmod 755 {SHELL_BIN}; rm -f {SHELL_DEV}/tawcroot-stats")
        stats_path = f"{SHELL_DEV}/tawcroot-stats"
    names = syscall_names()
    wanted = set(filter(None, opt.workloads.split(",")))
    selected = [w for w in WORKLOADS if not wanted or w[0] in wanted or w[0] == "empty"]

    rows = []
    for name, cmd in selected:
        run_workload(opt.install, opt.cpu, cmd, shell)   # warm-up
        a = snapshot(stats_path, shell)
        wall_us = run_workload(opt.install, opt.cpu, cmd, shell)
        b = snapshot(stats_path, shell)
        rows.append((name, wall_us, diff(a, b), b[1]))

    base = rows[0][2]
    for name, wall_us, d, freq in rows:
        tick_us = 1e6 / freq
        if name != "empty":
            d = [max(0, x - y) for x, y in zip(d, base)]
        counts = d[HEADER_WORDS:HEADER_WORDS + NR_MAX]
        ticks = d[HEADER_WORDS + NR_MAX:HEADER_WORDS + 2 * NR_MAX]
        inner = d[HEADER_WORDS + 2 * NR_MAX:HEADER_WORDS + 3 * NR_MAX]
        inner_by = d[HEADER_WORDS + 3 * NR_MAX:HEADER_WORDS + 4 * NR_MAX]
        traps = sum(counts)
        handler_us = sum(ticks) * tick_us
        execs, prep, children, init = d[2], d[3] * tick_us, d[4], d[5] * tick_us
        print(f"\n== {name}: wall {wall_us / 1000:.1f} ms | traps {traps}"
              f" | handler {handler_us / 1000:.1f} ms | execs {execs}"
              f" (prepare {prep / 1000:.1f} ms) | exec-child init {init / 1000:.1f} ms"
              f" over {children}")
        if d[48]:
            print(f"   path cache: {d[49]}/{d[48]} probes answered from cache")
        print(f"   /proc/self/fd readlinks {d[50]}: dirfd lift {d[51]},"
              f" fstat nlink {d[52]}, getdents {d[53]} (own), ro-bind {d[54]};"
              f" fd path cache hits {d[55]}")
        top_inner = sorted(range(NR_MAX), key=lambda n: -inner[n])[:8]
        print("   tawcroot's own syscalls:", ", ".join(
            f"{names.get(n, n)} {inner[n]}" for n in top_inner if inner[n]))
        if children:
            print("   exec-child init per child:", ", ".join(
                f"{p} {d[16 + i] * tick_us / children:.0f}us/{d[32 + i] / children:.1f}f"
                for i, p in enumerate(PHASES)))
        order = sorted(range(NR_MAX), key=lambda n: -ticks[n])[: opt.top]
        for n in order:
            if not counts[n]:
                continue
            print(f"   {names.get(n, n):>18} {counts[n]:>8} traps"
                  f" {ticks[n] * tick_us / 1000:8.2f} ms"
                  f" {ticks[n] * tick_us / counts[n]:7.2f} us/trap"
                  f" {inner_by[n] / counts[n]:5.1f} inner syscalls/trap")
    if shell:
        adb_shell(f"rm -f {SHELL_BIN} {SHELL_DEV}/tawcroot-stats")


if __name__ == "__main__":
    main()
