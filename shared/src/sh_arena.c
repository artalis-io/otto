/*
 * sh_arena.c - Arena allocator implementation
 */

#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>  /* For SIZE_MAX */

/*
 * ASan visibility for arena sub-allocations.
 *
 * The arena is one malloc'd block that hands out slices of itself. To ASan
 * that block is a single valid object, so a write that runs off the end of one
 * slice lands in the next slice and is not a heap overflow -- it is a perfectly
 * legal write to the middle of a live allocation. The result is that neither
 * ASan nor the fuzzer can see a whole class of bug in the arena's users, which
 * is every parser in shared/: sh_csv, sh_json, sh_xml and sh_pdf2struc.
 *
 * That is not theoretical. The overflow in decode_text_simple() that this
 * commit fixes sat in a file the fuzzer had been running over, and was found by
 * reading rather than by fuzzing, because the corrupted bytes belonged to
 * another arena slice.
 *
 * So: poison the whole buffer up front, unpoison exactly the bytes each
 * allocation returns, and leave a redzone poisoned between neighbours. An
 * overflow then reports as a use-after-poison at the first byte past the
 * slice, with the offending write at the top of the stack trace.
 *
 * Costs nothing in a normal build -- without ASan the macros compile away and
 * SH_ARENA_REDZONE is 0, so the allocation layout is byte-for-byte what it was.
 */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define SH_ARENA_ASAN 1
#  endif
#elif defined(__SANITIZE_ADDRESS__)
#  define SH_ARENA_ASAN 1
#endif

#ifdef SH_ARENA_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#  define SH_ARENA_POISON(p, n)   __asan_poison_memory_region((p), (n))
#  define SH_ARENA_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
/* A multiple of SH_ARENA_ALIGN so alignment of later slices is unchanged. */
#  define SH_ARENA_REDZONE SH_ARENA_ALIGN
#else
#  define SH_ARENA_POISON(p, n)   ((void)0)
#  define SH_ARENA_UNPOISON(p, n) ((void)0)
#  define SH_ARENA_REDZONE 0
#endif

SHArena *sh_arena_create(size_t capacity)
{
    SHArena *arena = malloc(sizeof(SHArena));
    if (!arena) return NULL;

    arena->buffer = malloc(capacity);
    if (!arena->buffer) {
        free(arena);
        return NULL;
    }

    arena->capacity = capacity;
    arena->used = 0;
    SH_ARENA_POISON(arena->buffer, capacity);
    return arena;
}

void *sh_arena_alloc(SHArena *arena, size_t size)
{
    /* Keep the caller's size: under ASan only the bytes actually asked for are
     * unpoisoned, so the alignment padding stays a redzone. Unpoisoning the
     * rounded-up size instead would let a small overflow land in the padding
     * and go unreported -- which is how a one-byte overflow in
     * pdf_parse_string() escaped the first version of this. */
    size_t want = size;
    (void)want;   /* only read under ASan */

    if (!arena || !arena->buffer) return NULL;

    /* Align to SH_ARENA_ALIGN bytes for double/pointer alignment */
    size = (size + SH_ARENA_ALIGN - 1) & ~(size_t)(SH_ARENA_ALIGN - 1);

    /* `size` is already rounded up, so this cannot wrap for any size the
     * capacity check would otherwise accept. */
    if (size > arena->capacity - arena->used) {
        return NULL;  /* Out of space */
    }

    void *ptr = arena->buffer + arena->used;
    arena->used += size;

#ifdef SH_ARENA_ASAN
    /* Keep the redzone inside the arena rather than past its end: if it will
     * not fit, the arena is full enough that the next alloc fails anyway.
     * Behind #ifdef because in a normal build the constant is 0 and the
     * comparison is unsigned >= 0, which -Wtype-limits rightly flags. */
    if (SH_ARENA_REDZONE <= arena->capacity - arena->used)
        arena->used += SH_ARENA_REDZONE;
#endif

    SH_ARENA_UNPOISON(ptr, want);
    return ptr;
}

void *sh_arena_calloc(SHArena *arena, size_t count, size_t size)
{
    /* Check for integer overflow before multiplication */
    if (size > 0 && count > SIZE_MAX / size) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = sh_arena_alloc(arena, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void sh_arena_reset(SHArena *arena)
{
    if (arena) {
        /* Re-poison so a pointer kept across a reset reports as use-after-free
         * rather than quietly aliasing whatever is handed out next. */
        SH_ARENA_POISON(arena->buffer, arena->capacity);
        arena->used = 0;
    }
}

void sh_arena_free(SHArena *arena)
{
    if (arena) {
        /* free() on a poisoned region is fine, but leaving it poisoned would
         * confuse a later allocation that reuses the same address. */
        SH_ARENA_UNPOISON(arena->buffer, arena->capacity);
        free(arena->buffer);
        free(arena);
    }
}

size_t sh_arena_remaining(const SHArena *arena)
{
    if (!arena) return 0;
    return arena->capacity - arena->used;
}

size_t sh_arena_used(const SHArena *arena)
{
    if (!arena) return 0;
    return arena->used;
}
