# Guest-filter smoke expectations are stale

`tawcroot/tests/testhost/src/rootfs_smoke.c:test_guest_seccomp_prctl_handling`
expects ENOSYS from a valid seccomp filter and EINVAL from its prctl
equivalent. `syscalls_control.c` deliberately validates and fake-accepts
both with success, without installing the guest filter.

These assertions fail on baseline `bb332e1`: host 13 failures, S24 10,
including repeated smoke wrappers and their exit-status assertions.
The exec-state experiment reproduced the same failures.

Update the stale assertions to match fake-accept semantics while retaining
the kill-filter test: installing that filter would terminate the testhost.
NULL capability probes and strict-mode behavior are separate contracts.
