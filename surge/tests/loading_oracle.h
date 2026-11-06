/* Independent specification oracle: enumerate final slot/layer coordinates.
 * Deliberately does not call the production search, validator or fit helpers. */
static int loading_oracle_valid(const SGLoadingVehicle *v, const SGLoadingUnit *u,
                                unsigned n, const SGLoadingPlacement *p) {
    uint64_t payload = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (p[i].slot >= v->slot_count || p[i].layer >= v->max_layers) return 0;
        payload += u[i].weight_g;
        for (unsigned j = 0; j < i; ++j)
            if (p[i].slot == p[j].slot && p[i].layer == p[j].layer) return 0;
    }
    if (payload > v->payload_g) return 0;
    for (unsigned slot = 0; slot < v->slot_count; ++slot) {
        uint64_t weight = 0, height = 0;
        unsigned mask = 0, count = 0;
        for (unsigned i = 0; i < n; ++i) if (p[i].slot == slot) {
            weight += u[i].weight_g; height += u[i].height_mm;
            mask |= 1u << p[i].layer; ++count;
        }
        if (weight > v->max_stack_weight_g || height > v->clearance_mm ||
            mask != (1u << count) - 1u) return 0;
    }
    for (unsigned i = 0; i < n; ++i) {
        uint64_t above_weight = 0;
        unsigned above_count = 0;
        for (unsigned j = 0; j < n; ++j) {
            if (p[i].slot == p[j].slot && p[j].layer > p[i].layer) {
                above_weight += u[j].weight_g; ++above_count;
                if (p[j].layer == p[i].layer + 1 &&
                    !(v->allowed_top[u[i].stack_class] & (UINT64_C(1) << u[j].stack_class))) return 0;
                if (v->access == SG_LOADING_REAR_LIFO && u[j].stop > u[i].stop) return 0;
            }
            if (v->access == SG_LOADING_REAR_LIFO &&
                v->lane[p[i].slot] == v->lane[p[j].slot] &&
                v->depth[p[i].slot] < v->depth[p[j].slot] && u[i].stop > u[j].stop) return 0;
        }
        if (above_weight > u[i].max_weight_above_g || above_count > u[i].max_units_above) return 0;
    }
    return 1;
}
static int loading_oracle_enumerate(const SGLoadingVehicle *v, const SGLoadingUnit *u,
                                    unsigned n, SGLoadingPlacement *p, unsigned i) {
    if (i == n) return loading_oracle_valid(v, u, n, p);
    for (unsigned slot = 0; slot < v->slot_count; ++slot)
        for (unsigned layer = 0; layer < v->max_layers; ++layer) {
            p[i] = (SGLoadingPlacement){slot, layer};
            if (loading_oracle_enumerate(v, u, n, p, i + 1)) return 1;
        }
    return 0;
}
static uint32_t loading_random(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}
static void randomized_loading_oracle(void) {
    const uint32_t seeds[] = {1, 42, UINT32_C(0x12345678), UINT32_MAX};
    unsigned cases = 0;
    for (unsigned seed = 0; seed < sizeof(seeds)/sizeof(*seeds); ++seed) {
        uint32_t state = seeds[seed];
        for (unsigned trial = 0; trial < 1024; ++trial) {
            SGLoadingVehicle v = {0}; SGLoadingUnit units[4] = {{0}};
            SGLoadingPlacement placement[4];
            unsigned n = loading_random(&state) % 5;
            v.slot_count = 1 + loading_random(&state) % 3;
            v.max_layers = 1 + loading_random(&state) % 3;
            v.class_count = 1 + loading_random(&state) % 3;
            v.access = (SGLoadingAccess)(loading_random(&state) % 2);
            v.clearance_mm = 500 + loading_random(&state) % 2501;
            v.max_stack_weight_g = 1 + loading_random(&state) % 2500;
            v.payload_g = 1 + loading_random(&state) % 3500;
            for (unsigned i = 0; i < v.slot_count; ++i) { v.lane[i] = i % 2; v.depth[i] = i / 2; }
            for (unsigned i = 0; i < v.class_count; ++i)
                v.allowed_top[i] = loading_random(&state) & ((1u << v.class_count) - 1u);
            for (unsigned i = 0; i < n; ++i) {
                units[i].stop = loading_random(&state) % 3;
                units[i].height_mm = 1 + loading_random(&state) % 1500;
                units[i].weight_g = loading_random(&state) % 1000;
                units[i].max_weight_above_g = loading_random(&state) % 2000;
                units[i].max_units_above = loading_random(&state) % 4;
                units[i].stack_class = loading_random(&state) % v.class_count;
            }
            int expected = loading_oracle_enumerate(&v, units, n, placement, 0);
            SGLoadingResult result;
            assert(sg_loading_check(&v, units, n, 100000, &result) == SG_STATUS_OK);
            if (result.outcome == SG_LOADING_UNKNOWN ||
                (result.outcome == SG_LOADING_FEASIBLE) != expected) {
                fprintf(stderr, "loading oracle mismatch seed=%u trial=%u\n", seeds[seed], trial);
                assert(0);
            }
            if (expected) assert(loading_oracle_valid(&v, units, n, result.placements));
            SGLoadingResult limited;
            assert(sg_loading_check(&v, units, n, 1 + trial % 3, &limited) == SG_STATUS_OK);
            assert(limited.outcome != SG_LOADING_INFEASIBLE || !expected);
            if (limited.outcome == SG_LOADING_FEASIBLE)
                assert(loading_oracle_valid(&v, units, n, limited.placements));
            ++cases;
        }
    }
    printf("loading: %u randomized independent exhaustive-oracle comparisons passed\n", cases);
}
