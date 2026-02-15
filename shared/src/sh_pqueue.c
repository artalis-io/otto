/*
 * sh_pqueue.c - Generic binary heap priority queue for arbitrary data types
 */

#include "sh_pqueue.h"
#include <stdlib.h>
#include <string.h>

/* Access element at index i */
#define ELEM_AT(h, i) ((char *)(h)->data + (i) * (h)->elem_size)

void sh_pqueue_init(SHPQueue *pqueue, size_t elem_size, SHPQueueCmp cmp)
{
    if (!pqueue) return;
    pqueue->data = NULL;
    pqueue->count = 0;
    pqueue->capacity = 0;
    pqueue->elem_size = elem_size;
    pqueue->cmp = cmp;
}

void sh_pqueue_free(SHPQueue *pqueue)
{
    if (!pqueue) return;
    free(pqueue->data);
    pqueue->data = NULL;
    pqueue->count = 0;
    pqueue->capacity = 0;
}

/* Swap elements at indices i and j */
static void swap_elems(SHPQueue *pqueue, size_t i, size_t j)
{
    if (i == j) return;

    char *a = ELEM_AT(pqueue, i);
    char *b = ELEM_AT(pqueue, j);

    /* Use stack buffer for small elements, heap for large */
    char stack_buf[256];
    char *tmp = pqueue->elem_size <= sizeof(stack_buf) ? stack_buf : malloc(pqueue->elem_size);
    if (!tmp) return;

    memcpy(tmp, a, pqueue->elem_size);
    memcpy(a, b, pqueue->elem_size);
    memcpy(b, tmp, pqueue->elem_size);

    if (tmp != stack_buf) free(tmp);
}

/* Bubble element at index i up toward the root */
static void bubble_up(SHPQueue *pqueue, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (pqueue->cmp(ELEM_AT(pqueue, parent), ELEM_AT(pqueue, i)) >= 0) break;
        swap_elems(pqueue, parent, i);
        i = parent;
    }
}

/* Bubble element at index i down toward the leaves */
static void bubble_down(SHPQueue *pqueue, size_t i)
{
    for (;;) {
        size_t left = 2 * i + 1;
        size_t right = 2 * i + 2;
        size_t best = i;

        if (left < pqueue->count &&
            pqueue->cmp(ELEM_AT(pqueue, left), ELEM_AT(pqueue, best)) > 0)
            best = left;
        if (right < pqueue->count &&
            pqueue->cmp(ELEM_AT(pqueue, right), ELEM_AT(pqueue, best)) > 0)
            best = right;

        if (best == i) break;
        swap_elems(pqueue, i, best);
        i = best;
    }
}

int sh_pqueue_push(SHPQueue *pqueue, const void *elem)
{
    if (!pqueue || !elem || !pqueue->cmp) return 0;

    if (pqueue->count >= pqueue->capacity) {
        size_t new_cap = pqueue->capacity ? pqueue->capacity * 2 : 64;

        /* Overflow check */
        if (new_cap < pqueue->capacity) return 0;
        if (pqueue->elem_size > 0 && new_cap > (size_t)-1 / pqueue->elem_size) return 0;

        void *new_data = realloc(pqueue->data, new_cap * pqueue->elem_size);
        if (!new_data) return 0;
        pqueue->data = new_data;
        pqueue->capacity = new_cap;
    }

    memcpy(ELEM_AT(pqueue, pqueue->count), elem, pqueue->elem_size);
    pqueue->count++;
    bubble_up(pqueue, pqueue->count - 1);

    return 1;
}

int sh_pqueue_pop(SHPQueue *pqueue, void *out)
{
    if (!pqueue || pqueue->count == 0) return 0;

    if (out) {
        memcpy(out, ELEM_AT(pqueue, 0), pqueue->elem_size);
    }

    pqueue->count--;
    if (pqueue->count > 0) {
        memcpy(ELEM_AT(pqueue, 0), ELEM_AT(pqueue, pqueue->count), pqueue->elem_size);
        bubble_down(pqueue, 0);
    }

    return 1;
}

int sh_pqueue_peek(const SHPQueue *pqueue, void *out)
{
    if (!pqueue || pqueue->count == 0 || !out) return 0;
    memcpy(out, (const char *)pqueue->data, pqueue->elem_size);
    return 1;
}

size_t sh_pqueue_count(const SHPQueue *pqueue)
{
    return pqueue ? pqueue->count : 0;
}

int sh_pqueue_empty(const SHPQueue *pqueue)
{
    return !pqueue || pqueue->count == 0;
}

void sh_pqueue_clear(SHPQueue *pqueue)
{
    if (pqueue) pqueue->count = 0;
}
