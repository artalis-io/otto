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

#if defined(__GNUC__) || defined(__clang__)
  /* Declared but possibly never referenced; do not warn. */
  #define SH_UNUSED   __attribute__((unused))
  /* Keep out of line -- used where a symbol must stay individually breakpointable
   * or separately measurable. */
  #define SH_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
  /* MSVC has no "unused" attribute; it does not warn about unreferenced statics
   * at /W3 either, so nothing is needed. */
  #define SH_UNUSED
  #define SH_NOINLINE __declspec(noinline)
#else
  #define SH_UNUSED
  #define SH_NOINLINE
#endif

#endif /* SH_ATTR_H */
