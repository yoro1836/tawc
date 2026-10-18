#!/usr/bin/env python3
"""Build an isolated, instrumented runtime; never stages into the APK.

Each successful exec emits one TAWC_EXEC_PROFILE stderr row at guest jump.
Fields are absolute CLOCK_MONOTONIC nanoseconds, grouped pre/post. The
debug-only state extension transports pre markers across kernel exec.
Commit uses its existing state mapping as MAP_SHARED/RW to publish them;
the final pre.exec timestamp follows that copy. Production sources stay intact.
Use this binary from a fresh invocation: its raw-stub address differs from
the production binary, but remains fixed across its own internal re-execs.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / "tawcroot"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--abi", choices=("aarch64", "x86_64"), default="aarch64")
parser.add_argument("--out", type=Path, default=REPO / "build/tawcroot-perf")
args = parser.parse_args()
out = args.out.resolve()
copy = out / "profile-src"
for subdir in ("src", "include"):
    shutil.copytree(SOURCE / subdir, copy / subdir, dirs_exist_ok=True)

header = r'''
#pragma once
#include <stdint.h>
#define TAWC_PROFILE_POINTS 32
struct tawc_profile_point { const char *name; uint64_t ns; };
struct tawc_profile_record {
    uint32_t magic, count;
    struct tawc_profile_point point[TAWC_PROFILE_POINTS];
};
void tawc_prof_reset(const char *kind);
void tawc_prof_kind(const char *kind);
void tawc_prof_mark(const char *name);
void tawc_prof_export(void *state);
void tawc_prof_import(const void *state);
void tawc_prof_emit(void);
'''
helper = r'''
#include <stddef.h>
#include "exec_state.h"
#include "raw_sys.h"
#include "tawc_string.h"
#define MAGIC 0x50524631u
static struct tawc_profile_record current, inherited;
static const char *kind;
static char line[4096];
static void stamp(struct tawc_profile_record *r, const char *name) {
    long ts[2];
    if (r->count >= TAWC_PROFILE_POINTS) return;
    if (TAWC_RAW(TAWC_SYS_clock_gettime, 1, (long)ts, 0, 0, 0, 0))
        tawc_exit_group(124);
    r->point[r->count].name = name;
    r->point[r->count++].ns = (uint64_t)ts[0] * 1000000000u + ts[1];
}
void tawc_prof_reset(const char *s) { current.count = 0; current.magic = MAGIC; kind = s; }
void tawc_prof_kind(const char *s) { kind = s; }
void tawc_prof_mark(const char *s) { stamp(&current, s); }
void tawc_prof_export(void *state) {
    tawcroot_exec_state_header *h = state;
    h->profile = current;
    stamp(&h->profile, "exec");
}
void tawc_prof_import(const void *state) {
    const tawcroot_exec_state_header *h = state;
    if (h->profile.magic == MAGIC && h->profile.count <= TAWC_PROFILE_POINTS)
        inherited = h->profile;
}
static char *string(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *number(char *p, uint64_t n) {
    char tmp[24]; unsigned i = 0;
    do { tmp[i++] = '0' + n % 10; n /= 10; } while (n);
    while (i) *p++ = tmp[--i];
    return p;
}
static char *points(char *p, const char *prefix, const struct tawc_profile_record *r) {
    for (unsigned i = 0; i < r->count; i++) {
        *p++ = '\t'; p = string(p, prefix); p = string(p, r->point[i].name);
        *p++ = '='; p = number(p, r->point[i].ns);
    }
    return p;
}
void tawc_prof_emit(void) {
    char *p = string(line, "TAWC_EXEC_PROFILE\tpid=");
    p = number(p, TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0));
    p = string(p, "\tkind="); p = string(p, kind);
    p = points(p, "pre.", &inherited); p = points(p, "post.", &current);
    *p++ = '\n';
    (void)TAWC_RAW(TAWC_SYS_write, 2, (long)line, (long)(p - line), 0, 0, 0);
}
'''
(copy / "include/exec_profile.h").write_text(header)
(copy / "src/exec_profile.c").write_text(helper)


def patch(file, old, new):
    path = copy / file
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit(f"source changed: {file}: expected one {old!r}")
    path.write_text(text.replace(old, new))


def mark(file, anchor, name, before=False):
    added = f'\n\ttawc_prof_mark("{name}");\n'
    patch("src/" + file, anchor, added + anchor if before else anchor + added)


for file in ("main.c", "syscalls_exec.c", "exec_handler.c", "loader_exec.c", "supervisor.c"):
    path = copy / "src" / file
    path.write_text('#include "exec_profile.h"\n' + path.read_text())
patch("include/exec_state.h", "#pragma once", '#pragma once\n#include "exec_profile.h"')
patch("include/exec_state.h", "} tawcroot_exec_state_header;",
      "\tstruct tawc_profile_record profile;\n} tawcroot_exec_state_header;")
patch("src/main.c", "void tawcroot_main(int argc, char **argv)\n{",
      'void tawcroot_main(int argc, char **argv)\n{\n\ttawc_prof_reset("init");\n\ttawc_prof_mark("main");')
mark("main.c", "enum tawcroot_entry entry = classify_entry(argc, argv);", "classified")
patch("src/main.c", "case ENTRY_EXEC_CHILD:\n", 'case ENTRY_EXEC_CHILD:\n\t\t\ttawc_prof_kind("child");\n')
patch("src/syscalls_exec.c", "static long handle_execve(const tawcroot_syscall_args *args, ucontext_t *uc)\n{",
      'static long handle_execve(const tawcroot_syscall_args *args, ucontext_t *uc)\n{\n\ttawc_prof_reset("pre");\n\ttawc_prof_mark("handler");')
patch("src/syscalls_exec.c", "static long handle_execveat(const tawcroot_syscall_args *args, ucontext_t *uc)\n{",
      'static long handle_execveat(const tawcroot_syscall_args *args, ucontext_t *uc)\n{\n\ttawc_prof_reset("pre");\n\ttawc_prof_mark("handler");')
mark("syscalls_exec.c", "size_t budget = (size_t)limit;", "collect_begin")
mark("syscalls_exec.c", "\trc = tawcroot_exec_handler_prepare(path, (int)argc, argv_ptrs, envp_ptrs);", "collect_done", True)
mark("syscalls_exec.c", "(void)tawc_munmap(strings, budget);", "strings_free")
mark("exec_handler.c", "if (!path || !argv || !envp || argc < 0) return TAWC_EINVAL;", "prepare_begin")
mark("exec_handler.c", '\tlong mfd = tawc_memfd_create("tawcroot-exec-state", 0);', "probe_done", True)
mark("exec_handler.c", "\tsize_t state_size = tawcroot_exec_state_estimate_bytes(path, argc,", "snapshot_done", True)
mark("exec_handler.c", "\treturn mfd;", "state_done", True)
mark("exec_handler.c", "tawc_munmap(scratch, sizeof *scratch);", "prepare_done")
mark("exec_handler.c", "static long commit(int mfd)\n{", "commit_begin")
patch("src/exec_handler.c", "long mrv = tawc_mmap((void *)0, (size_t)sz, TAWC_MM_PROT_READ,\n\t\t                     TAWC_MM_MAP_PRIVATE, (int)mfd, 0);",
      "long mrv = tawc_mmap((void *)0, (size_t)sz, TAWC_MM_PROT_READ | TAWC_MM_PROT_WRITE,\n\t\t                     1 /* MAP_SHARED: debug timing transport */, (int)mfd, 0);")
patch("src/exec_handler.c", "\tif (er >= 0)\n\t\ter = TAWC_RAW(TAWC_SYS_execve, (long)self_path, (long)new_argv,\n\t\t              (long)envp_for_self, 0, 0, 0);",
      '\tif (er >= 0) {\n\t\ttawc_prof_mark("commit_ready");\n\t\tif (map_rv >= 0) tawc_prof_export((void *)(uintptr_t)map_rv);\n\t\ter = TAWC_RAW(TAWC_SYS_execve, (long)self_path, (long)new_argv,\n\t\t              (long)envp_for_self, 0, 0, 0);\n\t}')
mark("loader_exec.c", "void tawcroot_loader_exec_child(int state_fd, const char *platform)\n{", "child_begin")
mark("loader_exec.c", "const void *buf = (const void *)mrv;", "state_mapped")
mark("loader_exec.c", "\t/* Phase 2g: re-establish tawcroot's per-process state in the new", "state_parsed", True)
patch("src/loader_exec.c", '\ttawc_prof_mark("state_parsed");', '\ttawc_prof_import(buf);\n\ttawc_prof_mark("state_parsed");')
mark("loader_exec.c", "\tstruct tawc_loader_exec_args ea = {", "restored", True)
mark("loader_exec.c", "void tawcroot_loader_exec(const struct tawc_loader_exec_args *args)\n{", "loader_begin")
mark("loader_exec.c", "\t/* --- 2. Map the binary. --- */", "elf_parsed", True)
mark("loader_exec.c", "\t/* --- 3. Optional: ld.so for dynamic guests. --- */", "elf_mapped", True)
mark("loader_exec.c", "\ttawc_close((int)bin_fd);\n\n\t/* --- 4. Allocate fresh stack. ---", "maps_done", True)
mark("loader_exec.c", "\tuint8_t random16[16];", "stack_reserved", True)
mark("loader_exec.c", "\t/* --- 5. Kernel-visible identity (comm + cmdline), then jump. ---", "stack_built", True)
patch("src/loader_exec.c", "\ttawc_loader_jump(so.sp, entry);", '\ttawc_prof_mark("jump");\n\ttawc_prof_emit();\n\ttawc_loader_jump(so.sp, entry);')
for anchor, name in (
    ("\t/* (3) usercopy probe", "root_ready"),
    ("\t/* (4) Bind table.", "usercopy_ready"),
    ("\t/* (5) Inherited /dev/shm", "binds_ready"),
    ("\ttawcroot_pathcache_init(args->store_host_path);", "store_ready"),
    ("\t/* (6) Memoise well-known", "cache_ready"),
    ("\ttawcroot_dispatch_init();", "memo_ready"),
    ("\t/* (7) Install the SIGSYS", "dispatch_ready"),
    ("\t/* (8) Reset the inherited", "handler_ready"),
    ("\t/* (9) Stash /proc/self/exe", "mask_ready"),
):
    mark("supervisor.c", anchor, name, True)
mark("supervisor.c", "tawcroot_init_self_host_path();", "supervisor_done")

build = (SOURCE / "build.sh").read_text()
def array(name):
    text = re.search(r"^" + name + r"=\((.*?)^\)", build, re.M | re.S).group(1)
    return [s.replace("$TAWCROOT_DIR", str(copy)) for s in shlex.split(text, comments=True)]
ndk = os.environ.get("ANDROID_NDK_HOME")
if not ndk:
    sdk = Path(os.environ.get("ANDROID_HOME", str(Path.home() / "Android/Sdk")))
    ndk = max((sdk / "ndk").iterdir(), key=lambda p: tuple(map(int, re.findall(r"\d+", p.name))))
bin_dir = Path(ndk) / "toolchains/llvm/prebuilt/linux-x86_64/bin"
cc = bin_dir / (args.abi + "-linux-android29-clang")
base = re.search(r'^ARCH_BASE_' + args.abi + r'="([^"]+)"', build, re.M).group(1)
cflags = array("COMMON_CFLAGS") + (["-mno-outline-atomics"] if args.abi == "aarch64" else [])
sources = array("SRC_C_PROD") + array("SRC_S_" + args.abi) + [str(copy / "src/exec_profile.c")]
objects = out / ("profile-obj-" + args.abi)
objects.mkdir(parents=True, exist_ok=True)
def compile_source(source):
    obj = objects / (Path(source).name + ".o")
    subprocess.run([str(cc), *cflags, "-g", "-c", source, "-o", str(obj)], check=True)
    return str(obj)
with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as pool:
    objs = list(pool.map(compile_source, sources))
binary = out / ("tawcroot-exec-profile-" + args.abi)
link = [str(cc), *array("COMMON_LDFLAGS"), "-Wl,--image-base=" + base,
        "-Wl,-soname,libtawcroot.so", *objs, "-o", str(binary)]
subprocess.run(link, check=True)
elf = subprocess.check_output([str(bin_dir / "llvm-readelf"), "-h", "-l", "-s", str(binary)], text=True)
if not re.search(r"Type:\s+EXEC", elf) or re.search(r"^\s+INTERP\s", elf, re.M):
    raise SystemExit("expected fixed-base ET_EXEC without PT_INTERP")
stub = next(line for line in elf.splitlines() if line.rstrip().endswith(" tawcroot_raw_syscall_ret"))
(out / (binary.name + ".json")).write_text(json.dumps({"binary": str(binary), "base": base,
    "raw_stub": stub.strip(), "compiler": str(cc), "cflags": cflags, "link": link}, indent=2) + "\n")
print(binary)
print(stub.strip())
