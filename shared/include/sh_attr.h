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

#endif /* SH_ATTR_H */
