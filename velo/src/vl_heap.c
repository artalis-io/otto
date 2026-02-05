/*
 * vl_heap.c - Thin wrapper around shared heap implementation
 *
 * Provides vl_heap_* API that wraps sh_heap_* functions.
 * VLHeap and VLHeapEntry are typedefs to SHHeap and SHHeapEntry.
 */

#include "vl_types.h"
#include "sh_heap.h"
#include <stdlib.h>

/* ============================================================================
 * Status Conversion
 * ============================================================================ */

static inline VLStatus convert_status(SHHeapStatus status)
{
    switch (status) {
        case SH_HEAP_OK:                 return VL_OK;
        case SH_HEAP_ERROR_NULL_PARAM:   return VL_ERROR_INVALID_ARGUMENT;
        case SH_HEAP_ERROR_OUT_OF_MEMORY: return VL_ERROR_OUT_OF_MEMORY;
        case SH_HEAP_ERROR_INVALID_NODE: return VL_ERROR_INVALID_ARGUMENT;
        case SH_HEAP_ERROR_EMPTY:        return VL_ERROR_INVALID_ARGUMENT;
        default:                         return VL_ERROR_INTERNAL;
    }
}

/* ============================================================================
 * Public API (wrappers around sh_heap)
 * ============================================================================ */

VLHeap *vl_heap_create(size_t num_nodes)
{
    return sh_heap_create(num_nodes);
}

void vl_heap_free(VLHeap *heap)
{
    sh_heap_free(heap);
}

void vl_heap_clear(VLHeap *heap)
{
    sh_heap_clear(heap);
}

int vl_heap_empty(const VLHeap *heap)
{
    return sh_heap_empty(heap);
}

size_t vl_heap_size(const VLHeap *heap)
{
    return sh_heap_size(heap);
}

int vl_heap_contains(const VLHeap *heap, uint32_t node)
{
    return sh_heap_contains(heap, node);
}

double vl_heap_priority(const VLHeap *heap, uint32_t node)
{
    return sh_heap_priority(heap, node);
}

VLStatus vl_heap_push(VLHeap *heap, uint32_t node, double priority)
{
    return convert_status(sh_heap_push(heap, node, priority));
}

VLStatus vl_heap_pop(VLHeap *heap, VLHeapEntry *entry)
{
    return convert_status(sh_heap_pop(heap, entry));
}

VLStatus vl_heap_peek(const VLHeap *heap, VLHeapEntry *entry)
{
    return convert_status(sh_heap_peek(heap, entry));
}

VLStatus vl_heap_decrease_key(VLHeap *heap, uint32_t node, double new_priority)
{
    return convert_status(sh_heap_decrease_key(heap, node, new_priority));
}

VLStatus vl_heap_push_or_decrease(VLHeap *heap, uint32_t node, double priority)
{
    return convert_status(sh_heap_push_or_decrease(heap, node, priority));
}
