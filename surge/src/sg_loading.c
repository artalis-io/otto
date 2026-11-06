#include "sg_loading.h"
#include <string.h>
#include <limits.h>

static int inputs_ok(const SGLoadingVehicle *v, const SGLoadingUnit *u, uint32_t n) {
    if (!v || (n && !u) || n > SG_LOADING_MAX_UNITS ||
        !v->slot_count || v->slot_count > SG_LOADING_MAX_SLOTS ||
        !v->max_layers || v->max_layers > SG_LOADING_MAX_LAYERS ||
        !v->clearance_mm || !v->max_stack_weight_g || !v->payload_g ||
        !v->class_count || v->class_count > SG_LOADING_MAX_CLASSES ||
        (v->access != SG_LOADING_SIDE_FREE && v->access != SG_LOADING_REAR_LIFO)) return 0;
    for (uint32_t i = 0; i < v->slot_count; ++i)
        for (uint32_t j = 0; j < i; ++j)
            if (v->lane[i] == v->lane[j] && v->depth[i] == v->depth[j]) return 0;
    for (uint32_t i = 0; i < v->class_count; ++i)
        if (v->class_count < 64 && (v->allowed_top[i] >> v->class_count)) return 0;
    for (uint32_t i = 0; i < n; ++i)
        if (!u[i].height_mm || u[i].stack_class >= v->class_count) return 0;
    return 1;
}

/* Deliberately recompute all geometry/support/access rules from the certificate,
 * independently of search state and pruning. Integer subtraction avoids overflow. */
SGStatus sg_loading_validate(const SGLoadingVehicle *v, const SGLoadingUnit *u,
                            uint32_t n, const SGLoadingPlacement *p) {
    if (!inputs_ok(v, u, n) || (n && !p)) return SG_STATUS_INVALID_ARG;
    uint64_t payload = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (p[i].slot >= v->slot_count || p[i].layer >= v->max_layers)
            return SG_STATUS_INFEASIBLE;
        if (u[i].weight_g > v->payload_g - payload) return SG_STATUS_INFEASIBLE;
        payload += u[i].weight_g;
    }
    for (uint32_t slot = 0; slot < v->slot_count; ++slot) {
        uint64_t weight = 0, height = 0;
        uint32_t mask = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (p[i].slot != slot) continue;
            uint32_t bit = UINT32_C(1) << p[i].layer;
            if (mask & bit) return SG_STATUS_INFEASIBLE;
            mask |= bit;
            if (u[i].weight_g > v->max_stack_weight_g - weight ||
                u[i].height_mm > v->clearance_mm - height) return SG_STATUS_INFEASIBLE;
            weight += u[i].weight_g; height += u[i].height_mm;
            uint64_t above = 0;
            uint32_t above_count = 0;
            for (uint32_t j = 0; j < n; ++j) {
                if (p[j].slot != slot || p[j].layer <= p[i].layer) continue;
                if (++above_count > u[i].max_units_above ||
                    u[j].weight_g > u[i].max_weight_above_g - above) return SG_STATUS_INFEASIBLE;
                above += u[j].weight_g;
                if (p[j].layer == p[i].layer + 1 &&
                    !(v->allowed_top[u[i].stack_class] & (UINT64_C(1) << u[j].stack_class)))
                    return SG_STATUS_INFEASIBLE;
                if (v->access == SG_LOADING_REAR_LIFO && u[j].stop > u[i].stop)
                    return SG_STATUS_INFEASIBLE;
            }
        }
        if (mask & (mask + 1)) return SG_STATUS_INFEASIBLE; /* no floating layers */
    }
    if (v->access == SG_LOADING_REAR_LIFO)
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = 0; j < n; ++j)
                if (v->lane[p[i].slot] == v->lane[p[j].slot] &&
                    v->depth[p[i].slot] < v->depth[p[j].slot] && u[i].stop > u[j].stop)
                    return SG_STATUS_INFEASIBLE;
    return SG_STATUS_OK;
}

typedef struct {
    const SGLoadingVehicle *v;
    const SGLoadingUnit *u;
    uint32_t n;
    uint64_t budget, nodes;
    int exhausted;
    uint32_t order[SG_LOADING_MAX_UNITS];
    uint32_t used[SG_LOADING_MAX_SLOTS];
    uint32_t stack[SG_LOADING_MAX_SLOTS][SG_LOADING_MAX_LAYERS];
    uint64_t height[SG_LOADING_MAX_SLOTS], weight[SG_LOADING_MAX_SLOTS];
    SGLoadingPlacement placements[SG_LOADING_MAX_UNITS];
} Search;

static int fits(const Search *s, uint32_t id, uint32_t slot) {
    const SGLoadingVehicle *v = s->v;
    const SGLoadingUnit *u = &s->u[id];
    uint32_t layers = s->used[slot];
    if (layers >= v->max_layers || u->height_mm > v->clearance_mm - s->height[slot] ||
        u->weight_g > v->max_stack_weight_g - s->weight[slot]) return 0;
    if (layers && !(v->allowed_top[s->u[s->stack[slot][layers-1]].stack_class] &
                     (UINT64_C(1) << u->stack_class))) return 0;
    uint64_t above = u->weight_g;
    for (uint32_t k = layers; k > 0; --k) {
        const SGLoadingUnit *bottom = &s->u[s->stack[slot][k-1]];
        if (layers - (k-1) > bottom->max_units_above || above > bottom->max_weight_above_g) return 0;
        /* Stack total already bounded, hence this cannot overflow. */
        above += bottom->weight_g;
    }
    if (v->access == SG_LOADING_REAR_LIFO) {
        for (uint32_t j = 0; j < layers; ++j)
            if (u->stop > s->u[s->stack[slot][j]].stop) return 0;
        for (uint32_t other = 0; other < v->slot_count; ++other) {
            if (other == slot || v->lane[other] != v->lane[slot]) continue;
            for (uint32_t k = 0; k < s->used[other]; ++k) {
                uint32_t stop = s->u[s->stack[other][k]].stop;
                if ((v->depth[other] < v->depth[slot] && stop > u->stop) ||
                    (v->depth[other] > v->depth[slot] && stop < u->stop)) return 0;
            }
        }
    }
    return 1;
}

static int search(Search *s, uint32_t pos) {
    if (s->nodes == s->budget) { s->exhausted = 1; return 0; }
    ++s->nodes;
    if (pos == s->n) return 1;
    for (uint32_t pick = pos; pick < s->n; ++pick) {
        uint32_t id = s->order[pick];
        s->order[pick] = s->order[pos]; s->order[pos] = id;
        for (uint32_t slot = 0; slot < s->v->slot_count; ++slot) {
            if (!fits(s, id, slot)) continue;
            uint32_t layer = s->used[slot]++;
            s->stack[slot][layer] = id;
            s->placements[id] = (SGLoadingPlacement){slot, layer};
            s->height[slot] += s->u[id].height_mm;
            s->weight[slot] += s->u[id].weight_g;
            if (search(s, pos + 1)) return 1;
            --s->used[slot];
            s->height[slot] -= s->u[id].height_mm;
            s->weight[slot] -= s->u[id].weight_g;
            if (s->exhausted) return 0;
        }
        s->order[pos] = s->order[pick]; s->order[pick] = id;
    }
    return 0;
}

SGStatus sg_loading_check(const SGLoadingVehicle *v, const SGLoadingUnit *u,
                         uint32_t n, uint64_t budget, SGLoadingResult *r) {
    if (!r) return SG_STATUS_INVALID_ARG;
    memset(r, 0, sizeof(*r)); r->outcome = SG_LOADING_UNKNOWN;
    r->reason = SG_LOADING_UNSUPPORTED;
    if (!inputs_ok(v, u, n)) return SG_STATUS_INVALID_ARG;
    uint64_t payload = 0, total_height = 0;
    for (uint32_t i = 0; i < n; ++i) {
        total_height += u[i].height_mm;
        if (u[i].weight_g > v->payload_g - payload ||
            u[i].height_mm > v->clearance_mm || u[i].weight_g > v->max_stack_weight_g)
            goto capacity;
        payload += u[i].weight_g;
    }
    if (n > v->slot_count * v->max_layers ||
        total_height > (uint64_t)v->slot_count * v->clearance_mm) goto capacity;
    Search s = {0}; s.v = v; s.u = u; s.n = n; s.budget = budget;
    for (uint32_t i = 0; i < n; ++i) s.order[i] = i;
    /* Every legal stack has a bottom-to-top order sorted by nonincreasing stop
     * in rear mode. Both modes explore vertical permutations too; search
     * below does so by choosing the next unit, rather than assuming an order. */
    if (v->access == SG_LOADING_REAR_LIFO)
        for (uint32_t i = 1; i < n; ++i) {
            uint32_t id = s.order[i], j = i;
            while (j && u[s.order[j-1]].stop < u[id].stop) { s.order[j] = s.order[j-1]; --j; }
            s.order[j] = id;
        }
    if (search(&s, 0)) {
        r->nodes = s.nodes;
        if (sg_loading_validate(v, u, n, s.placements) != SG_STATUS_OK) return SG_STATUS_ERROR;
        r->outcome = SG_LOADING_FEASIBLE; r->reason = SG_LOADING_WITNESS; r->count = n;
        memcpy(r->placements, s.placements, n * sizeof(*r->placements));
    } else {
        r->nodes = s.nodes;
        r->outcome = s.exhausted ? SG_LOADING_UNKNOWN : SG_LOADING_INFEASIBLE;
        r->reason = s.exhausted ? SG_LOADING_BUDGET : SG_LOADING_NO_ARRANGEMENT;
    }
    return SG_STATUS_OK;
capacity:
    r->outcome = SG_LOADING_INFEASIBLE; r->reason = SG_LOADING_CAPACITY;
    return SG_STATUS_OK;
}
