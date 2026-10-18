/* SIGSYS bypass for aarch64 (on unless built with
 * -DTAWCROOT_FASTPATH=0; see fastpath_config.h).
 *
 * A guest syscall site that keeps trapping for a uc-free handler is
 * rewritten from `svc #0` to a `b` into a per-site stub, which calls
 * the dispatcher directly (tawcroot_fast_common, arch asm) instead of
 * going through signal delivery. Only sites of the form
 * `mov {w,x}8, #nr; svc #0` with nr on a fixed allowlist are patched,
 * so the syscall number at a patched site is statically known.
 *
 * TAWCROOT_FASTPATH=1 blocks signals around the direct call, as the
 * SIGSYS handler's sa_mask does, but only once the process has caught a
 * signal: until then every disposition is default or ignore, so no guest
 * code can run in the middle of a handler. =2 never blocks, for
 * measuring the ceiling only — it is not reentrancy-safe.
 */

#pragma once

#include <ucontext.h>

#include "dispatch.h"
#include "fastpath_config.h"

#if defined(__aarch64__) && TAWCROOT_FASTPATH
/* Called from the SIGSYS handler after a trap was dispatched. */
void tawcroot_fastpath_note(const tawcroot_syscall_args *args, ucontext_t *uc);
/* rt_sigaction installed `handler` for a guest signal. Sticky: a vfork
 * child's later reset must never clear what the parent still has. */
void tawcroot_fastpath_note_handler(unsigned long handler);
#else
static inline void tawcroot_fastpath_note(const tawcroot_syscall_args *args,
					  ucontext_t *uc)
{
	(void)args;
	(void)uc;
}
static inline void tawcroot_fastpath_note_handler(unsigned long handler)
{
	(void)handler;
}
#endif
