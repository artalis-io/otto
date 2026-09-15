/*
 * sh_attr.h - Compiler attribute portability
 *
 * GCC and Clang spell these as __attribute__; MSVC either spells them
 * differently or has no equivalent. Same shape lp_log.h already uses for its
 * printf attribute: emit the attribute where it exists, nothing where it does
 * not.
 *
 * Nothing here changes behaviour. These only silence warnings or pin inlining,
 * so an empty definition is always a correct fallback.
 */

#ifndef SH_ATTR_H
#define SH_ATTR_H

/* Struct packing.
 *
 * GCC and Clang take an attribute on the struct; MSVC has no attribute and
 * uses #pragma pack around the definitions instead. A macro cannot emit the
 * pragma pair, so a header defining packed layouts brackets its own region --
 * see locus/include/lc_mmap.h -- and tags each struct with SH_PACKED so the
 * GNU side still gets its attribute. Both mechanisms coexist: whichever
 * compiler is in use, exactly one of them does the work.
 *
 * Unlike the rest of this header, this one is NOT cosmetic: these describe
 * on-disk layouts, and getting it wrong misreads every file written by the
 * other compiler.
 */
/* printf-style format checking.
 *
 * On MinGW, format(printf, ...) means Microsoft's printf, which has no %z --
 * so a size_t argument that is correct on every other platform is reported as
 * an error there. gnu_printf is the checker that matches the ANSI-compliant
 * printf MinGW actually links, and it is what MinGW's own headers select.
 *
 * Keyed on __MINGW32__ rather than __MINGW_PRINTF_FORMAT because the latter
 * arrives with <stdio.h>: a header included before it would silently get the
 * Microsoft checker and the error this exists to prevent.
 */
#if defined(__MINGW32__)
  #define SH_PRINTF_ATTR(fmt_idx, args_idx) \
      __attribute__((format(gnu_printf, fmt_idx, args_idx)))
#elif defined(__GNUC__) || defined(__clang__)
  #define SH_PRINTF_ATTR(fmt_idx, args_idx) \
      __attribute__((format(printf, fmt_idx, args_idx)))
#else
  #define SH_PRINTF_ATTR(fmt_idx, args_idx)
#endif

#if defined(__GNUC__) || defined(__clang__)
  /* Declared but possibly never referenced; do not warn. */
  #define SH_PACKED   __attribute__((packed))
  #define SH_UNUSED   __attribute__((unused))
  /* Keep out of line -- used where a symbol must stay individually breakpointable
   * or separately measurable. */
  #define SH_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
  /* MSVC has no "unused" attribute; it does not warn about unreferenced statics
   * at /W3 either, so nothing is needed. */
  /* Emitted by #pragma pack at the definition site instead. */
  #define SH_PACKED
  #define SH_UNUSED
  #define SH_NOINLINE __declspec(noinline)
#else
  #define SH_PACKED
  #define SH_UNUSED
  #define SH_NOINLINE
#endif

/* Thread-local storage.
 *
 * Unlike the rest of this header, this one is NOT cosmetic -- the same caveat
 * SH_PACKED carries. Every other macro here can expand to nothing and the code
 * still behaves identically; an empty SH_THREAD_LOCAL turns a per-thread
 * variable into a shared one, which is a data race rather than a missing
 * optimisation. That is why the last branch warns instead of failing silently.
 *
 * C11 spells it _Thread_local and both GCC/Clang and MSVC predate that with
 * their own spellings. Prefer the standard one where the compiler claims C11
 * with threads.
 *
 * Consolidated from three identical ladders that had drifted apart:
 * FW_THREAD_LOCAL (fuelwise/src/fw_refuel.c), CS_THREAD_LOCAL
 * (clayshards/clay-shards/src/cs_internal.h) and SH_THREAD_LOCAL
 * (shared/src/sh_httpserver.c). The ClayShards copy was the only one that
 * warned on the no-TLS fallback; that behaviour is kept here for all callers.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
  #define SH_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
  #define SH_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
  #define SH_THREAD_LOCAL __declspec(thread)
#else
  #define SH_THREAD_LOCAL
  #define SH_NO_TLS 1
  #if defined(__GNUC__) || defined(__clang__)
    #warning "No TLS support: variables tagged SH_THREAD_LOCAL are shared across threads"
  #elif defined(_MSC_VER)
    #pragma message("No TLS support: variables tagged SH_THREAD_LOCAL are shared across threads")
  #endif
#endif

#endif /* SH_ATTR_H */
