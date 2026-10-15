/* config.h — hand-written for PlutoBrowser (replaces the Meson-generated
 * file from dav1d 1.5.1). One configuration serves BOTH targets (the muJS
 * rule: shared flags so the sim validates the exact code the device runs).
 *
 * Deliberate choices (SIMPLEST viable AVIF):
 * - No assembly anywhere (all ARCH_* 0): pure C DSP on both targets.
 *   The Playdate has no NEON/SSE; the sim uses C paths too so timings and
 *   code paths match the device (only the clock differs).
 * - 8-bit only (CONFIG_8BPC, no CONFIG_16BPC): the 13 *_tmpl.c DSP files
 *   compile once with -DBITDEPTH=8 (see Makefile). 10/12-bit AVIFs are
 *   refused cleanly by the decoder (NULL → placeholder card), never
 *   misdecoded. This is an upstream-supported configuration.
 * - Single-threaded decode (n_threads=1, max_frame_delay=1 in the
 *   wrapper): every thread primitive in dav1d is gated on n_tc > 1 (see
 *   lib.c), so no pthread port is needed. pthread_once/attr stubs still
 *   link (qjs_pthread_stubs.c precedent); they execute, mutex/cond/create
 *   never do.
 * - Feature probes are the INTERSECTION of macOS-sim and newlib-device:
 *   nothing requiring unistd, dlsym, memalign, clock_gettime, or
 *   pthread_np. malloc/free fallback paths are used throughout.
 */
#pragma once

#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_X86 0
#define ARCH_X86_32 0
#define ARCH_X86_64 0

#define CONFIG_8BPC 1
/* CONFIG_16BPC deliberately undefined: 8-bit only (see above). */

#define CONFIG_LOG 1
#define CONFIG_MACOS_KPERF 0
#define ENDIANNESS_BIG 0

#define HAVE_ALIGNED_ALLOC 0 /* newlib's aligned_alloc links against a
 * missing posix_memalign on bare metal (link failure) — do not enable. */
#define HAVE_MEMALIGN 1 /* newlib memalign (declared in malloc.h, freed by
 * plain free) covers dav1d's 32/64B frame-buffer alignment. */
#define HAVE_ASM 0
#define HAVE_AS_ARCHEXT_DOTPROD_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_I8MM_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_SVE2_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_SVE_DIRECTIVE 0
#define HAVE_AS_ARCH_DIRECTIVE 0
#define HAVE_AS_FUNC 0
#define HAVE_C11_GENERIC 1
#define HAVE_CLOCK_GETTIME 0
#define HAVE_DLSYM 0
#define HAVE_DOTPROD 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_GETAUXVAL 0
#define HAVE_I8MM 0
#define HAVE_IO_H 0
#define HAVE_POSIX_MEMALIGN 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_NP_H 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define HAVE_SVE 0
#define HAVE_SVE2 0
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 0
#define PREFIX 1
#define TRIM_DSP_FUNCTIONS 1
