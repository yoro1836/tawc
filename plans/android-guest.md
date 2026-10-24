# Android guest: Waydroid images on tawcroot

Run a full Android userspace rootless inside the TAWC app: an unmodified
Waydroid LineageOS image booted by `tawcroot --init`, with binder emulated
in userspace and the display going through the TAWC compositor.

Rule of thumb: **kernel interfaces are emulated in tawcroot** (works for any
image, including redroid), **Android policy problems get ROM patches** (only
when emulation would be absurd, built on Hetzner, see "ROM builds").

## Image facts (verified)

Checked against `lineage-20.0-20260927-VANILLA-waydroid_arm64-system.zip` and
`lineage-20.0-20261022-MAINLINE-waydroid_arm64-vendor.zip` from
`ota.waydro.id` (the OTA JSON carries sha256 per file).

- Android 13, userdebug, security patch 2026-02. Raw ext4: system 1.9 GB,
  vendor 0.4 GB. Extractable without root (`debugfs rdump`).
- System-as-root: the system image root is the rootfs root, `/init` is a
  symlink into `/system/bin/init`.
- **APEX is flattened** (`/system/apex/<name>/` directories): no apexd
  loop/dm-verity.
- `ro.zygote=zygote64_32`, abilist32 present. tawcroot has no AArch32, so
  start with `ro.zygote=zygote64` (64-bit apps only).
- HALs are mostly HIDL: composer 2.1 (`hwcomposer.waydroid`), allocator
  2.0/4.0 (`gralloc.default`, `gralloc.gbm`, minigbm), audio 4.0 (PulseAudio
  socket), health/light/power/sensors/vibrator `*.waydroid`. Waydroid HIDL:
  `vendor.waydroid.{display,window,task,clipboard}`.
- `hwcomposer.waydroid` is a Wayland client (`WAYLAND_DISPLAY`,
  `XDG_RUNTIME_DIR`, xdg-shell). Buffer paths: `wl_shm`, dmabuf, and
  `android_wlegl` (already served by `compositor/src/wlegl.rs`). Supports
  `persist.waydroid.multi_windows` (one toplevel per Android task).
- Input does not need uinput: the HWC writes Wayland input into
  `/dev/input/wl_{touch,pointer,keyboard,tablet}_events` for InputFlinger.
- Container-aware init: `mount_all /dev/null`, bind+remount of `/`.
  Vendor early-init stops the allocator when `/dev/host_hwbinder` exists.

## Host contract (what Waydroid's host side provides)

From `waydroid/waydroid` `tools/helpers/{lxc,images}.py`. This is the
checklist the TAWC side has to cover.

| Waydroid host | TAWC equivalent |
|---|---|
| binderfs `binder`/`hwbinder`/`vndbinder` nodes | binderd + tawcroot binder emulation |
| `/dev/ashmem` | not needed: `sys.use_memfd=true` |
| tmpfs `/dev`, devpts, `/dev/{null,zero,full,tty,fuse,uhid,tun,sw_sync,ion}`, GPU nodes | tawcroot device table; absent nodes stay absent |
| LXC caps: sys_admin, net_admin, setuid/setgid, mknod, chown, dac_override, … | tawcroot virtual root/identity, mount/cgroup emulation |
| `/vendor/waydroid.prop` (bind file, base props) | written by the installer into the rootfs |
| Wayland socket → `/run/user/1000/wayland-0` | bind of the TAWC compositor socket |
| Pulse socket | later (see `plans/audio.md`) |
| `data` dir bind | per-instance dir under app data |
| Host binder services `waydroidclipboard`, `waydroidhardware`, `waydroidnotifications`, `waydroidusermonitor`; calls guest `waydroidplatform` | host-services bridge registered in binderd |
| bridge + dnsmasq, guest `eth0` | **open**: no netns rootless, see risks |

## Architecture

```
TAWC app process (app uid)
├─ Kotlin: install UI, session service, host-services bridge (client of binderd)
├─ compositor (Rust, Wayland server)  ◄── wayland-0 ──┐
├─ binderd (one per instance)                          │
│    contexts: binder / hwbinder / vndbinder           │
│    nodes, refs, txn stacks, death notifications      │
└─ tawcroot --init  ──►  /init (pid 1, emulated)       │
       └─ servicemanager, hwservicemanager, surfaceflinger,
          composer HAL (hwcomposer.waydroid) ──────────┘
          zygote64 → system_server → apps
          every guest process: SIGSYS handler + binder shim
```

### Binder emulation

- Guest `open("/dev/{binder,hwbinder,vndbinder}")` returns a **memfd** (the
  receive buffer, so the guest's untrapped `mmap` just works) duplicated to
  fd ≥ 1000 so its `close` traps. The handler keeps a side table:
  binder fd → binderd process connection (reserved fd), RW alias mapping.
- `ioctl` on a binder fd is serviced over AF_UNIX `SOCK_SEQPACKET`: one
  process connection, plus one lazily opened connection per thread (binder
  state is per-thread). FDs in transactions travel via `SCM_RIGHTS`; the
  receiving handler installs them and patches the buffer through its alias.
- binderd writes payloads straight into the receiver's memfd. The guest
  mapping base/size is found once from `/proc/self/maps` and sent to binderd.
- Blocking reads wait on {thread socket, signalfd(guest unblocked mask)};
  a pending guest signal returns `-EINTR` with `write_consumed` updated
  (libbinder's `talkWithDriver` retries). Same idea as `src/sysv_sem.c`.
- Sender pid/uid come in-band from tawcroot's virtual identity/pid emulation,
  not `SO_PEERCRED`.
- Scope: BC/BR core set, refcount protocol (INCREFS/ACQUIRE/…), death
  notifications, objects BINDER/HANDLE/FD/FDA/PTR (scatter-gather for HIDL),
  `SET_CONTEXT_MGR_EXT`, `TRANSACTION_SG`, A13 ioctls (`FREEZE`,
  `GET_EXTENDED_ERROR`, …), binderfs `features/` files.
- binderd also exposes a host API so the Kotlin bridge can register the
  `waydroid*` services and call `waydroidplatform`.

## Code layout

| Path | Contents |
|---|---|
| `tawcroot/include/binder_wire.h` | binderd ↔ shim wire protocol (shared) |
| `tawcroot/src/binder.c` | in-handler shim: open/ioctl/close hooks, memfd buffers, per-thread connections |
| `tawcroot/binderd/` | daemon: ordinary libc C, static; NDK build like `ando/`, host build for tests |
| `tawcroot/tests/hosted/test_binder_*.c`, `tests/integration/` | protocol tests, then real guests talking through binderd |
| `app/…` new distro kind `android` | OTA fetch + sha256, ext4 extract, prop overlay, session start (binderd + `tawcroot --init`), host-services bridge |
| `scripts/android-guest/` | dev helpers (host-side extract, push, boot log capture) |
| `rom/` (later) | Waydroid `local_manifests` patches + Hetzner build scripts |

## Boot flow

1. Install: fetch system/vendor zips, verify sha256, extract system → rootfs,
   vendor → `rootfs/vendor`.
2. Prepare: `/vendor/waydroid.prop` (base props + `ro.zygote=zygote64`,
   `sys.use_memfd=true`, `ro.hardware.gralloc/egl` for the chosen GPU path),
   synthesized `/proc/cmdline`, instance `data` dir.
3. Start: binderd, then `tawcroot --init --cgroup -r <rootfs> -b <wayland socket>:/run/user/1000/wayland-0 -b <data>:/data -- /init`.
4. Iterate on whatever init/servicemanager/surfaceflinger trip over.

## Emulate vs patch (first guess, the boot log decides)

- tawcroot: binder ×3, mount/remount/pivot, cgroups, setuid/setgroups/capset,
  nested seccomp (zygote installs one per app: accept and drop), prctl,
  sched/ioprio, `/proc` and `/sys` files Android reads, uevent netlink
  (empty), `/dev/kmsg`, `/dev/pmsg0`.
- ROM patch candidates: networking (ConnectivityService wants a netd-managed
  default network), FUSE storage (`/dev/fuse` mount is impossible rootless),
  vold, lmkd, 32-bit zygote if ever needed.

## Milestones

1. binderd + shim, host tests: transaction/reply, refcounts, fd passing,
   death notification, scatter-gather.
2. On device: `servicemanager` + `hwservicemanager` + `service list` under
   tawcroot (needs a device target).
3. surfaceflinger + composer HAL → first frame in the TAWC compositor.
4. system_server + launcher + input; freeze the ROM-patch list.
5. Host-services bridge (clipboard, notifications, app list), install UI.
6. Own ROM builds only if milestone 4 requires them; redroid `_64only` as a
   second image family.

## ROM builds

Hetzner Cloud, hourly: create a CPX51-class server (16 vCPU, 32 GB RAM,
360 GB disk; add swap), sync Waydroid's LineageOS tree, build, pull
artifacts, delete the server. Keep a snapshot only when iterating often.
Deleting is what stops billing; script it.

## Risks

- **Phantom process killer**: Android 12+ trims app child processes past 32
  system-wide ([issue](../issues/phantom-process-killer-kills-rootfs-processes.md)).
  An Android guest runs 60+ processes. Likely needs the user to disable it
  (adb `settings put global settings_enable_monitor_phantom_procs false`
  on 12L+) or a workaround; blocking for real use.
- Binder cost: every call is SIGSYS + socket round trips + two copies.
  Fallback: patch libbinder in our ROM to talk to binderd directly.
- Host `untrusted_app` SELinux limits (netlink, ioctls) surface as odd errnos.
- Networking without a netns (see above).
- 16 KB page kernels: the guest's 4 KB-aligned ELFs will not load.
- Memory: a booted Android 13 needs roughly 1.5–2 GB.

## Open decisions

- Product shell: build this as a distro kind inside the TAWC app (reuses
  install flow, sessions, compositor, multi-activity windows) and decide
  later whether lilac/VineOS consumes the pieces, or port to lilac now.
