/* TAWCROOT_FASTPATH default (fastpath.h). On for aarch64 builds; pass
 * -DTAWCROOT_FASTPATH=0 to turn it off. Stats builds default it off:
 * rewritten sites bypass the SIGSYS handler, so their traps would not
 * be counted. Kept apart from fastpath.h so the arch assembly can
 * include it. */
#pragma once

#if !defined(TAWCROOT_FASTPATH) && defined(__aarch64__)
# ifdef TAWCROOT_STATS
#  define TAWCROOT_FASTPATH 0
# else
#  define TAWCROOT_FASTPATH 1
# endif
#endif
