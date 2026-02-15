/*
 * sh_dheap.c - Generic binary heap for arbitrary data types
 */

#include "sh_dheap.h"
#include <stdlib.h>
#include <string.h>

/* Access element at index i */
#define ELEM_AT(h, i) ((char *)(h)->data + (i) * (h)->elem_size)

void sh_dheap_init(SHDHeap *heap, size_t elem_size, SHDHeapCmp cmp)
{
    if (!heap) return;
    heap->data = NULL;
    heap->count = 0;
    heap->capacity = 0;
    heap->elem_size = elem_size;
    heap->cmp = cmp;
}

void sh_dheap_free(SHDHeap *heap)
{
    if (!heap) return;
    free(heap->data);
    heap->data = NULL;
    heap->count = 0;
    heap->capacity = 0;
}

/* Swap elements at indices i and j */
static void swap_elems(SHDHeap *heap, size_t i, size_t j)
{
    if (i == j) return;

    char *a = ELEM_AT(heap, i);
    char *b = ELEM_AT(heap, j);

    /* Use stack buffer for small elements, heap for large */
    char stack_buf[256];
    char *tmp = heap->elem_size <= sizeof(stack_buf) ? stack_buf : malloc(heap->elem_size);
    if (!tmp) return;

    memcpy(tmp, a, heap->elem_size);
    memcpy(a, b, heap->elem_size);
    memcpy(b, tmp, heap->elem_size);

    if (tmp != stack_buf) free(tmp);
}

/* Bubble element at index i up toward the root */
static void bubble_up(SHDHeap *heap, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (heap->cmp(ELEM_AT(heap, parent), ELEM_AT(heap, i)) >= 0) break;
        swap_elems(heap, parent, i);
        i = parent;
    }
}

/* Bubble element at index i down toward the leaves */
static void bubble_down(SHDHeap *heap, size_t i)
{
    for (;;) {
        size_t left = 2 * i + 1;
        size_t right = 2 * i + 2;
        size_t best = i;

        if (left < heap->count &&
            heap->cmp(ELEM_AT(heap, left), ELEM_AT(heap, best)) > 0)
            best = left;
        if (right < heap->count &&
            heap->cmp(ELEM_AT(heap, right), ELEM_AT(heap, best)) > 0)
            best = right;

        if (best == i) break;
        swap_elems(heap, i, best);
        i = best;
    }
}

int sh_dheap_push(SHDHeap *heap, const void *elem)
{
    if (!heap || !elem || !heap->cmp) return 0;

    if (heap->count >= heap->capacity) {
        size_t new_cap = heap->capacity ? heap->capacity * 2 : 64;

        /* Overflow check */
        if (new_cap < heap->capacity) return 0;
        if (heap->elem_size > 0 && new_cap > (size_t)-1 / heap->elem_size) return 0;

        void *new_data = realloc(heap->data, new_cap * heap->elem_size);
        if (!new_data) return 0;
        heap->data = new_data;
        heap->capacity = new_cap;
    }

    memcpy(ELEM_AT(heap, heap->count), elem, heap->elem_size);
    heap->count++;
    bubble_up(heap, heap->count - 1);

    return 1;
}

int sh_dheap_pop(SHDHeap *heap, void *out)
{
    if (!heap || heap->count == 0) return 0;

    if (out) {
        memcpy(out, ELEM_AT(heap, 0), heap->elem_size);
    }

    heap->count--;
    if (heap->count > 0) {
        memcpy(ELEM_AT(heap, 0), ELEM_AT(heap, heap->count), heap->elem_size);
        bubble_down(heap, 0);
    }

    return 1;
}

int sh_dheap_peek(const SHDHeap *heap, void *out)
{
    if (!heap || heap->count == 0 || !out) return 0;
    memcpy(out, (const char *)heap->data, heap->elem_size);
    return 1;
}

size_t sh_dheap_count(const SHDHeap *heap)
{
    return heap ? heap->count : 0;
}

int sh_dheap_empty(const SHDHeap *heap)
{
    return !heap || heap->count == 0;
}

void sh_dheap_clear(SHDHeap *heap)
{
    if (heap) heap->count = 0;
}
