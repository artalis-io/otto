/*
 * sg_neighbor.c — k-nearest location index for insertion pruning.
 *
 * Build: For each (layer, location) pair, scan all other locations and
 * maintain a max-heap of size k to find the k smallest distances.
 * O(P * n^2 * log k) total where P = num_profiles + 1, n = num_locations.
 *
 * Query: Binary search in the sorted neighbor list.  O(log k) per lookup.
 */

#include "sg_neighbor.h"
#include "../include/sg_internal.h"
#include <stdlib.h>
#include <string.h>

/* ────────── Max-heap helpers for top-k selection ────────── */

typedef struct {
    double dist;
    uint32_t loc;
} HeapEntry;

static void heap_sift_down(HeapEntry *heap, uint32_t size, uint32_t i) {
    while (1) {
        uint32_t largest = i;
        uint32_t left  = 2 * i + 1;
        uint32_t right = 2 * i + 2;
        if (left < size && heap[left].dist > heap[largest].dist)
            largest = left;
        if (right < size && heap[right].dist > heap[largest].dist)
            largest = right;
        if (largest == i) break;
        { HeapEntry tmp = heap[i]; heap[i] = heap[largest]; heap[largest] = tmp; }
        i = largest;
    }
}

static void heap_push(HeapEntry *heap, uint32_t *size, uint32_t cap,
                       double dist, uint32_t loc) {
    if (*size < cap) {
        heap[*size] = (HeapEntry){ dist, loc };
        /* sift up */
        {
            uint32_t c = *size;
            while (c > 0) {
                uint32_t p = (c - 1) / 2;
                if (heap[c].dist <= heap[p].dist) break;
                { HeapEntry tmp = heap[c]; heap[c] = heap[p]; heap[p] = tmp; }
                c = p;
            }
        }
        (*size)++;
    } else if (dist < heap[0].dist) {
        heap[0] = (HeapEntry){ dist, loc };
        heap_sift_down(heap, cap, 0);
    }
}

/* Sort heap entries by location id (for binary search in is_near). */
static int cmp_by_loc(const void *a, const void *b) {
    uint32_t la = ((const HeapEntry *)a)->loc;
    uint32_t lb = ((const HeapEntry *)b)->loc;
    return (la > lb) - (la < lb);
}

/* ────────── Build one layer of neighbors from a distance matrix ────────── */

static void build_layer(uint32_t *out, const double *dist_matrix,
                        uint32_t n, uint32_t k, HeapEntry *heap) {
    uint32_t a, b, j;
    for (a = 0; a < n; a++) {
        uint32_t hsize = 0;
        for (b = 0; b < n; b++) {
            if (b == a) continue;
            heap_push(heap, &hsize, k, dist_matrix[(size_t)a * n + b], b);
        }
        /* Sort by location for binary search */
        qsort(heap, hsize, sizeof(HeapEntry), cmp_by_loc);
        for (j = 0; j < hsize; j++)
            out[(size_t)a * k + j] = heap[j].loc;
        /* Pad remaining with UINT32_MAX sentinel */
        for (j = hsize; j < k; j++)
            out[(size_t)a * k + j] = UINT32_MAX;
    }
}

/* ────────── Resolve the distance matrix for a travel profile ────────── */

static const double *profile_dist_matrix(const SGContext *ctx, uint32_t profile_idx) {
    const SGTravelProfile *tp = &ctx->travel_profiles[profile_idx];
    if (tp->has_time_brackets && tp->time_brackets[0].distance_matrix)
        return tp->time_brackets[0].distance_matrix;
    if (tp->has_distance_matrix)
        return tp->distance_matrix;
    /* Fall back to global */
    return NULL;
}

static const double *global_dist_matrix(const SGContext *ctx) {
    if (ctx->has_travel_time_brackets &&
        ctx->travel_time_brackets[0].distance_matrix)
        return ctx->travel_time_brackets[0].distance_matrix;
    return ctx->travel_distance_matrix;
}

/* ────────── Public API ────────── */

void sg_neighbor_init(SGNeighborIndex *idx, const SGContext *ctx, uint32_t k) {
    uint32_t n, num_profiles, num_entries, p;
    const double *gdm;
    HeapEntry *heap;

    memset(idx, 0, sizeof(*idx));

    /* Skip build for callback mode or degenerate cases */
    if (!ctx || ctx->travel_callback || ctx->num_locations < 2 || k == 0)
        return;

    n = ctx->num_locations;
    if (k > n - 1) k = n - 1;

    num_profiles = ctx->has_travel_profiles ? ctx->num_travel_profiles : 0;
    num_entries = n * (1 + num_profiles);

    idx->neighbors = (uint32_t *)malloc((size_t)num_entries * k * sizeof(uint32_t));
    if (!idx->neighbors) return;

    idx->num_entries   = num_entries;
    idx->num_locations = n;
    idx->k             = k;
    idx->num_profiles  = num_profiles;

    heap = (HeapEntry *)malloc(k * sizeof(HeapEntry));
    if (!heap) {
        free(idx->neighbors);
        memset(idx, 0, sizeof(*idx));
        return;
    }

    /* Layer 0: global distance matrix */
    gdm = global_dist_matrix(ctx);
    build_layer(idx->neighbors, gdm, n, k, heap);

    /* Layers 1..num_profiles: per-profile distance matrices */
    for (p = 0; p < num_profiles; p++) {
        const double *pdm = profile_dist_matrix(ctx, p);
        if (!pdm) pdm = gdm;  /* profile has no distance matrix → use global */
        build_layer(idx->neighbors + (size_t)(p + 1) * n * k, pdm, n, k, heap);
    }

    free(heap);
}

void sg_neighbor_free(SGNeighborIndex *idx) {
    if (idx) {
        free(idx->neighbors);
        memset(idx, 0, sizeof(*idx));
    }
}

int sg_neighbor_is_near(const SGNeighborIndex *idx, const SGContext *ctx,
                        uint32_t vehicle_id, uint32_t a, uint32_t b) {
    const uint32_t *row;
    uint32_t layer, lo, hi;

    if (!idx->neighbors) return 1;  /* disabled → assume near */
    if (a == b) return 1;
    if (a >= idx->num_locations || b >= idx->num_locations) return 1;

    /* Select layer: global (0) or per-profile */
    layer = 0;
    if (idx->num_profiles > 0 && ctx->has_travel_profiles &&
        vehicle_id != SG_NO_VEHICLE) {
        uint32_t tp_id = ctx->vehicles[vehicle_id].travel_profile_id;
        if (tp_id > 0 && tp_id <= idx->num_profiles)
            layer = tp_id;  /* profiles are 1-indexed → layer = tp_id */
    }

    row = idx->neighbors + ((size_t)layer * idx->num_locations + a) * idx->k;

    /* Binary search for b in sorted neighbor list */
    lo = 0;
    hi = idx->k;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (row[mid] == UINT32_MAX) {
            hi = mid;
        } else if (row[mid] < b) {
            lo = mid + 1;
        } else if (row[mid] > b) {
            hi = mid;
        } else {
            return 1;  /* found */
        }
    }
    return 0;
}

int sg_neighbor_vehicle_has_nearby(const SGNeighborIndex *idx, const SGContext *ctx,
                                   const SGRouteSolution *sol, uint32_t vehicle_id,
                                   uint32_t request_id) {
    uint32_t len, s;
    uint32_t loc_p, loc_d;
    int is_pd;
    const SGRequestRecord *req;

    if (!idx->neighbors) return 1;  /* disabled → evaluate all */

    len = sol->route_stop_lengths[vehicle_id];
    if (len == 0) return 1;  /* empty vehicles are never pruned */

    req = &ctx->requests[request_id];
    is_pd = (req->kind == SG_REQUEST_KIND_PICKUP_DELIVERY);

    if (is_pd) {
        loc_p = ctx->tasks[req->pickup_task_id].location_id;
        loc_d = ctx->tasks[req->delivery_task_id].location_id;
    } else {
        loc_d = ctx->tasks[req->delivery_task_id].location_id;
        loc_p = UINT32_MAX;  /* sentinel — only check delivery */
    }

    /* Check each stop on the vehicle's route */
    {
        size_t base = (size_t)vehicle_id * sol->stop_stride;
        for (s = 0; s < len; s++) {
            uint32_t stop_loc = ctx->tasks[sol->route_stops[base + s].task_id].location_id;
            if (sg_neighbor_is_near(idx, ctx, vehicle_id, loc_d, stop_loc))
                return 1;
            if (loc_p != UINT32_MAX &&
                sg_neighbor_is_near(idx, ctx, vehicle_id, loc_p, stop_loc))
                return 1;
        }
    }

    /* Also check depot locations (vehicle start/end) */
    {
        uint32_t start_loc = ctx->vehicles[vehicle_id].start_location_id;
        uint32_t end_loc   = ctx->vehicles[vehicle_id].end_location_id;
        if (sg_neighbor_is_near(idx, ctx, vehicle_id, loc_d, start_loc))
            return 1;
        if (loc_p != UINT32_MAX &&
            sg_neighbor_is_near(idx, ctx, vehicle_id, loc_p, start_loc))
            return 1;
        if (end_loc != start_loc) {
            if (sg_neighbor_is_near(idx, ctx, vehicle_id, loc_d, end_loc))
                return 1;
            if (loc_p != UINT32_MAX &&
                sg_neighbor_is_near(idx, ctx, vehicle_id, loc_p, end_loc))
                return 1;
        }
    }

    return 0;
}
