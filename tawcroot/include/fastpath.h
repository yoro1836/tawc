/* Experimental SIGSYS bypass (TAWCROOT_FASTPATH builds only, aarch64).
 *
 * A guest syscall site that keeps trapping for a uc-free handler is
 * rewritten from `svc #0` to a `b` into a per-site stub, which calls
 * the dispatcher directly (tawcroot_fast_common, arch asm) instead of
 * going through signal delivery. Only sites of the form
 * `mov {w,x}8, #nr; svc #0` with nr on a fixed allowlist are patched,
 * so the syscall number at a patched site is statically known.
 *
 * TAWCROOT_FASTPATH=1 blocks signals around the direct call (the SIGSYS
 * handler runs with them blocked); =2 does not, for measuring the
 * ceiling only — it is not reentrancy-safe.
 */

#pragma once

#include <ucontext.h>

#include "dispatch.h"

#if defined(TAWCROOT_FASTPATH) && defined(__aarch64__)
/* Called from the SIGSYS handler after a trap was dispatched. */
void tawcroot_fastpath_note(const tawcroot_syscall_args *args, ucontext_t *uc);
#else
static inline void tawcroot_fastpath_note(const tawcroot_syscall_args *args,
					  ucontext_t *uc)
{
	(void)args;
	(void)uc;
}
#endif
