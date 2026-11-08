#include "sg_axle.h"
#include <string.h>

static int coordinate(int32_t value) {
    return value >= -SG_AXLE_MAX_COORD_MM && value <= SG_AXLE_MAX_COORD_MM;
}
static int mass_interval(SGAxleMassInterval v) {
    return v.min <= v.max && v.max <= SG_AXLE_MAX_MASS_G;
}
/* Compare positive-denominator fractions without cross-product overflow.
 * Equal integral parts reduce to their remainders; inversion reverses order.
 * Euclidean reduction terminates even at UINT64_MAX. No floating point, wide
 * compiler extension, allocation or approximate inequality is required. */
static int fraction_ge(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    int reverse = 0;
    for (;;) {
        uint64_t qa = a / b, qc = c / d;
        if (qa != qc) return reverse ? qa < qc : qa > qc;
        uint64_t ra = a % b, rc = c % d;
        if (!ra || !rc) {
            if (!ra && !rc) return 1;
            return reverse ? ra == 0 : rc == 0;
        }
        a = b; b = ra; c = d; d = rc;
        reverse = !reverse;
    }
}

SGStatus sg_axle_profile_validate(const SGLoadingVehicle *v, const SGAxleProfile *p) {
    if (!v || !p || !v->slot_count || v->slot_count > SG_LOADING_MAX_SLOTS ||
        p->slot_count != v->slot_count || !p->wheelbase_mm ||
        p->wheelbase_mm > SG_AXLE_MAX_COORD_MM || !p->footprint_length_mm ||
        p->footprint_length_mm > SG_AXLE_MAX_COORD_MM ||
        !coordinate(p->deck_min_mm) || !coordinate(p->deck_max_mm) ||
        p->deck_min_mm >= p->deck_max_mm ||
        !mass_interval(p->front_tare_g) || !mass_interval(p->rear_tare_g) ||
        !mass_interval(p->front_limits_g) || !mass_interval(p->rear_limits_g) ||
        p->max_gross_g > SG_AXLE_MAX_MASS_G || p->has_min_front_fraction > 1 ||
        (p->has_min_front_fraction &&
            (!p->min_front_fraction.denominator ||
             p->min_front_fraction.denominator > 1000000 ||
             p->min_front_fraction.numerator > p->min_front_fraction.denominator)) ||
        (!p->has_min_front_fraction &&
            (p->min_front_fraction.numerator || p->min_front_fraction.denominator)) ||
        (v->access != SG_LOADING_SIDE_FREE && v->access != SG_LOADING_REAR_LIFO))
        return SG_STATUS_INVALID_ARG;
    for (uint32_t i = 0; i < p->slot_count; i++) {
        int64_t x = p->slot_centres_mm[i];
        if (!coordinate(p->slot_centres_mm[i]) ||
            2*x - p->footprint_length_mm < 2*(int64_t)p->deck_min_mm ||
            2*x + p->footprint_length_mm > 2*(int64_t)p->deck_max_mm)
            return SG_STATUS_INVALID_ARG;
        for (uint32_t j = 0; j < i; j++) {
            if (v->lane[i] != v->lane[j]) continue;
            int64_t distance = x - p->slot_centres_mm[j];
            if (distance < 0) distance = -distance;
            if (distance < p->footprint_length_mm || v->depth[i] == v->depth[j])
                return SG_STATUS_INVALID_ARG;
            if (v->access == SG_LOADING_REAR_LIFO &&
                ((v->depth[i] < v->depth[j]) != (x > p->slot_centres_mm[j])))
                return SG_STATUS_INVALID_ARG;
        }
    }
    return SG_STATUS_OK;
}

SGStatus sg_axle_state(const SGLoadingVehicle *v, const SGAxleProfile *p,
    const SGLoadingUnit *u, const SGAxleSignedInterval *cg,
    const uint32_t *slots, const uint8_t *active, uint32_t n, SGAxleState *out) {
    if (!out) return SG_STATUS_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (sg_axle_profile_validate(v, p) != SG_STATUS_OK || n > SG_LOADING_MAX_UNITS ||
        (n && (!u || !cg || !slots || !active))) return SG_STATUS_INVALID_ARG;
    int64_t min_moment = 0, max_moment = 0;
    uint64_t mass = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (u[i].weight_g > SG_AXLE_MAX_MASS_G || slots[i] >= p->slot_count || active[i] > 1 ||
            !coordinate(cg[i].min) || !coordinate(cg[i].max) || cg[i].min > cg[i].max ||
            2*(int64_t)cg[i].min < -(int64_t)p->footprint_length_mm ||
            2*(int64_t)cg[i].max > p->footprint_length_mm)
            return SG_STATUS_INVALID_ARG;
        if (!active[i]) continue;
        int64_t x = p->slot_centres_mm[slots[i]], w = (int64_t)u[i].weight_g;
        /* Validated caps prove |sum(w*x)| <= 128 * 10^9 * 10^6.
         * Scaled reactions remain < 2.57e17, strictly inside signed int64. */
        min_moment += w * (x + cg[i].min);
        max_moment += w * (x + cg[i].max);
        mass += u[i].weight_g;
    }
    int64_t length = p->wheelbase_mm;
    SGAxleState state = {0};
    state.denominator = p->wheelbase_mm;
    state.front_min_num = ((int64_t)p->front_tare_g.min + (int64_t)mass)*length - max_moment;
    state.front_max_num = ((int64_t)p->front_tare_g.max + (int64_t)mass)*length - min_moment;
    state.rear_min_num = (int64_t)p->rear_tare_g.min*length + min_moment;
    state.rear_max_num = (int64_t)p->rear_tare_g.max*length + max_moment;
    state.gross_min_g = p->front_tare_g.min + p->rear_tare_g.min + mass;
    state.gross_max_g = p->front_tare_g.max + p->rear_tare_g.max + mass;
    *out = state;
    if (state.front_min_num < (int64_t)p->front_limits_g.min*length ||
        state.front_max_num > (int64_t)p->front_limits_g.max*length ||
        state.rear_min_num < (int64_t)p->rear_limits_g.min*length ||
        state.rear_max_num > (int64_t)p->rear_limits_g.max*length ||
        state.gross_max_g > p->max_gross_g) return SG_STATUS_INFEASIBLE;
    if (p->has_min_front_fraction) {
        /* q*front - p*gross is minimized at front tare min, rear tare max
         * and maximum cargo moment: q-p >= 0, -p <= 0 and -q < 0. */
        uint64_t gross = p->front_tare_g.min + p->rear_tare_g.max + mass;
        if (gross && !fraction_ge((uint64_t)state.front_min_num, gross*(uint64_t)length,
                p->min_front_fraction.numerator, p->min_front_fraction.denominator))
            return SG_STATUS_INFEASIBLE;
    }
    return SG_STATUS_OK;
}

#include "sg_loading_search.h"

/* A whole unit must be topmost and reachable. Side access removes the lane-path
 * restriction, not the need to remove upper units before their support. */
static int removable(const SGLoadingVehicle *v, const SGLoadingPlacement *p,
                     const uint8_t *active, uint32_t n, uint32_t id) {
    for (uint32_t j = 0; j < n; j++) {
        if (!active[j] || j == id) continue;
        if (p[j].slot == p[id].slot && p[j].layer > p[id].layer) return 0;
        if (v->access == SG_LOADING_REAR_LIFO &&
            v->lane[p[j].slot] == v->lane[p[id].slot] &&
            v->depth[p[j].slot] < v->depth[p[id].slot]) return 0;
    }
    return 1;
}

static SGStatus joint_state(const SGLoadingVehicle *v, const SGAxleProfile *profile,
    const SGLoadingUnit *units, const SGAxleSignedInterval *cg, const uint32_t *slots,
    const uint8_t *active, uint32_t count, SGAxleState *out) {
    if (profile) return sg_axle_state(v,profile,units,cg,slots,active,count,out);
    memset(out,0,sizeof(*out));return SG_STATUS_OK;
}

SGStatus sg_axle_loading_validate(const SGLoadingVehicle *v, const SGAxleProfile *profile,
    const SGLoadingUnit *u, const SGAxleSignedInterval *cg, uint32_t n,
    const SGLoadingPlacement *p, const uint32_t *order, SGAxleState *states) {
    if (n > SG_LOADING_MAX_UNITS || (n && (!u || (profile && !cg) || !p || !order)) || (!profile && cg))
        return SG_STATUS_INVALID_ARG;
    SGStatus status = sg_loading_validate_joint(v, u, n, p);
    if (status != SG_STATUS_OK) return status;
    uint32_t slots[SG_LOADING_MAX_UNITS] = {0};uint8_t active[SG_LOADING_MAX_UNITS] = {0};
    SGAxleState checked[SG_LOADING_MAX_UNITS+1];
    for (uint32_t i = 0; i < n; i++) { slots[i] = p[i].slot;active[i] = 1; }
    status = joint_state(v, profile, u, cg, slots, active, n, &checked[0]);
    if (status != SG_STATUS_OK) return status;
    for (uint32_t step = 0; step < n; step++) {
        uint32_t id = order[step];
        if (id >= n || !active[id]) return SG_STATUS_INFEASIBLE;
        for (uint32_t j = 0; j < n; j++)
            if (active[j] && u[j].stop < u[id].stop) return SG_STATUS_INFEASIBLE;
        if (!removable(v, p, active, n, id)) return SG_STATUS_INFEASIBLE;
        active[id] = 0;
        status = joint_state(v, profile, u, cg, slots, active, n, &checked[step+1]);
        if (status != SG_STATUS_OK) return status;
    }
    /* Reversing these valid removals gives bottom-up, far-before-near loading
     * into the original positions, visiting exactly the same static states. */
    if (states) memcpy(states, checked, (n+1)*sizeof(*states));
    return SG_STATUS_OK;
}

typedef struct {
    const SGLoadingVehicle *v;
    const SGAxleProfile *profile;
    const SGLoadingUnit *units;
    const SGAxleSignedInterval *cg;
    const SGLoadingPlacement *placements;
    uint32_t count, slots[SG_LOADING_MAX_UNITS], order[SG_LOADING_MAX_UNITS];
    uint8_t active[SG_LOADING_MAX_UNITS];
    SGAxleState states[SG_LOADING_MAX_UNITS+1];
} AxleSearch;

static int unload(AxleSearch *s, uint32_t step, SGLoadingWork *work) {
    if (step == s->count) return 1;
    uint32_t next_stop = UINT32_MAX;
    for (uint32_t i = 0; i < s->count; i++)
        if (s->active[i] && s->units[i].stop < next_stop) next_stop = s->units[i].stop;
    for (uint32_t i = 0; i < s->count; i++) {
        if (!s->active[i] || s->units[i].stop != next_stop ||
            !removable(s->v, s->placements, s->active, s->count, i)) continue;
        if (!sg_loading_work_enter(work)) return 0;
        s->active[i] = 0;
        SGStatus status = joint_state(s->v,s->profile,s->units,s->cg,s->slots,
                                       s->active,s->count,&s->states[step+1]);
        s->order[step] = i;
        if (status == SG_STATUS_OK && unload(s,step+1,work)) return 1;
        s->active[i] = 1;
        if (work->exhausted) return 0;
    }
    return 0;
}
static int axle_leaf(const SGLoadingPlacement *p, SGLoadingWork *work, void *data) {
    AxleSearch *s = data;s->placements = p;
    for (uint32_t i = 0; i < s->count; i++) { s->slots[i] = p[i].slot;s->active[i] = 1; }
    if (joint_state(s->v,s->profile,s->units,s->cg,s->slots,s->active,s->count,
                      &s->states[0]) != SG_STATUS_OK) return 0;
    return unload(s,0,work);
}
SGStatus sg_axle_loading_check(const SGLoadingVehicle *v, const SGAxleProfile *profile,
    const SGLoadingUnit *u, const SGAxleSignedInterval *cg, uint32_t n,
    uint64_t budget, SGLoadingShouldStop stop, void *stop_data, SGAxleLoadingResult *out) {
    if (!out) return SG_STATUS_INVALID_ARG;
    memset(out,0,sizeof(*out));out->loading.outcome = SG_LOADING_UNKNOWN;
    out->loading.reason = SG_LOADING_UNSUPPORTED;
    if (n > SG_LOADING_MAX_UNITS || (n && (!u || (profile && !cg))) || (!profile && cg) || budget > 100000)
        return SG_STATUS_INVALID_ARG;
    SGLoadingResult physical;
    SGStatus physical_status = sg_loading_check_joint(v,u,n,0,NULL,NULL,NULL,NULL,&physical);
    if (physical_status != SG_STATUS_OK) return physical_status;
    uint32_t empty_slots[SG_LOADING_MAX_UNITS] = {0};
    uint8_t empty[SG_LOADING_MAX_UNITS] = {0};SGAxleState tare;
    SGStatus status = joint_state(v,profile,u,cg,empty_slots,empty,n,&tare);
    if (status == SG_STATUS_INVALID_ARG) return status;
    if (physical.outcome == SG_LOADING_INFEASIBLE) {
        out->loading = physical;return SG_STATUS_OK;
    }
    /* Tare-only failure is independent of packing or unloading choices. */
    if (status == SG_STATUS_INFEASIBLE) {
        out->loading.outcome = SG_LOADING_INFEASIBLE;out->loading.reason = SG_LOADING_CAPACITY;
        return SG_STATUS_OK;
    }
    AxleSearch search = {0};
    search.v=v;search.profile=profile;search.units=u;search.cg=cg;search.count=n;
    status = sg_loading_check_joint(v,u,n,budget,stop,stop_data,axle_leaf,&search,&out->loading);
    if (status != SG_STATUS_OK || out->loading.outcome != SG_LOADING_FEASIBLE) return status;
    status = sg_axle_loading_validate(v,profile,u,cg,n,out->loading.placements,search.order,out->states);
    if (status != SG_STATUS_OK) {
        memset(out,0,sizeof(*out));out->loading.outcome=SG_LOADING_UNKNOWN;
        out->loading.reason=SG_LOADING_UNSUPPORTED;return SG_STATUS_ERROR;
    }
    memcpy(out->unload_order,search.order,n*sizeof(*out->unload_order));
    return SG_STATUS_OK;
}
