#include "sg_parallel.h"
#include "sh_json.h"
#include "sg_api.h" /* Requires sh_json.h first for writer declarations. */
#include "surge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SGContext *base(void) {
    SGContext *ctx = sg_create();
    assert(ctx);
    SGConfig config;
    sg_config_default(&config);
    config.max_iterations = 40;
    config.max_time_seconds = 2;
    config.seed = 42;
    assert(sg_set_config(ctx, &config) == SG_STATUS_OK);
    uint32_t depot = sg_add_depot(ctx);
    assert(depot == 0);
    assert(sg_depot_set_location(ctx, depot, 0, 0) == SG_STATUS_OK);
    assert(sg_add_vehicle(ctx) == 0);
    assert(sg_vehicle_set_depots(ctx, 0, 0, 0) == SG_STATUS_OK);
    double cap = 2000;
    assert(sg_vehicle_set_capacity(ctx, 0, &cap, 1) == SG_STATUS_OK);
    assert(sg_add_delivery_request(ctx, 0, 0, 0, 100000, 60, -500) == 0);
    return ctx;
}
static void configure(SGContext *ctx) {
    SGLoadingV2Config c = {sizeof(c), 2, SG_LOADING_V2_STACKING | SG_LOADING_V2_AXLES, 10000, 0};
    assert(sg_loading_v2_set_config(ctx, &c) == SG_STATUS_OK);
    SGLoadingVehicle physical = {0};
    physical.slot_count = 1;
    physical.max_layers = 2;
    physical.clearance_mm = 2400;
    physical.class_count = 1;
    physical.allowed_top[0] = 1;
    physical.max_stack_weight_g = 1000000;
    physical.payload_g = 10000000;
    int32_t centres[1] = {2000};
    SGLoadingV2Axles axles = {0};
    axles.model = SG_LOADING_V2_RIGID_TWO_AXLE;
    axles.profile_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    axles.wheelbase_mm = 4000;
    axles.footprint_length_mm = 1200;
    axles.deck_min_mm = 0;
    axles.deck_max_mm = 8000;
    axles.slot_count = 1;
    axles.slot_centres_mm = centres;
    axles.front_tare_g = (SGLoadingV2MassInterval){3000000, 3000000};
    axles.rear_tare_g = (SGLoadingV2MassInterval){2000000, 2000000};
    axles.front_limits_g = (SGLoadingV2MassInterval){0, 6000000};
    axles.rear_limits_g = (SGLoadingV2MassInterval){0, 6500000};
    axles.max_gross_g = 12000000;
    SGLoadingV2Vehicle v = {physical, &axles, NULL};
    assert(sg_loading_v2_set_vehicle(ctx, 0, &v) == SG_STATUS_OK);
    centres[0] = INT32_MIN;
    axles.wheelbase_mm = 0; // inputs are deep copied
    SGLoadingV2SignedInterval cg = {-50, 50};
    SGLoadingV2Unit unit = {0};
    unit.physical.height_mm = 1000;
    unit.physical.weight_g = 500000;
    unit.physical.max_weight_above_g = 500000;
    unit.physical.max_units_above = 1;
    unit.cg_offset_mm = &cg;
    assert(sg_loading_v2_set_request(ctx, 0, &unit, 1) == SG_STATUS_OK);
    cg.min = 5000;
    unit.physical.weight_g = UINT64_MAX;
    SGStatus valid = sg_validate_model(ctx);
    if (valid != SG_STATUS_OK)
        fprintf(stderr, "model: %d %s\n", valid, sg_get_last_error(ctx));
    assert(valid == SG_STATUS_OK);
    assert(sg_set_hard_loading(ctx, 0, 0) == SG_STATUS_INVALID_ARG);
}
static const SGLoadingV2RoundEvidence *view(SGLoadingV2Report *r) {
    uint32_t count, diagnostic_count;
    const SGLoadingV2RoundEvidence *rounds;
    const SGLoadingV2Diagnostic *diagnostics;
    assert(sg_loading_v2_report_view(r, &count, &rounds, &diagnostic_count, &diagnostics) ==
           SG_STATUS_OK);
    assert(count == 1 && diagnostic_count == 0 && rounds[0].outcome == SG_LOADING_FEASIBLE);
    assert(rounds[0].placement_count == 1 && rounds[0].operation_count == 2 &&
           rounds[0].axle_state_count == 3);
    assert(rounds[0].axle_states[1].front_min_g.numerator == INT64_C(12975000000));
    assert(rounds[0].stops[0].departure_sec == 60);
    return rounds;
}
static int cancel_check(void *data) {
    (void)data;
    return 1;
}
int main(void) {
    SGLoadingV2Capabilities caps = {0};
    caps.struct_size = sizeof(caps);
    caps.schema_version = 2;
    assert(sg_loading_v2_capabilities(&caps) == SG_STATUS_OK);
    assert(caps.implemented_capabilities == (SG_LOADING_V2_STACKING | SG_LOADING_V2_AXLES));
    SGContext *ctx = base();
    SGLoadingV2Config unsupported = {sizeof(unsupported), 2, 7, 10000, 256};
    assert(sg_loading_v2_set_config(ctx, &unsupported) == SG_STATUS_NOT_IMPLEMENTED);
    configure(ctx);
    SGLoadingV2Report *report = NULL;
    assert(sg_loading_v2_solution_report(ctx, &report) == SG_STATUS_ERROR && report == NULL);
    SGPopulationConfig population = {2, 4, 2, 0.5};
    SGStatus status = sg_solve_population(ctx, &population);
    if (status != SG_STATUS_OK && status != SG_STATUS_LIMIT)
        fprintf(stderr, "HGS status=%d: %s\n", status, sg_get_last_error(ctx));
    assert(status == SG_STATUS_OK || status == SG_STATUS_LIMIT);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_loading_v2_solution_report(ctx, &report) == SG_STATUS_OK);
    const SGLoadingV2RoundEvidence *evidence = view(report);
    SGLoadingV2ReportInfo info;
    assert(sg_loading_v2_report_info(report, &info) == SG_STATUS_OK);
    uint32_t task = 0;
    SGLoadingV2Round round = {0, 0, 1, &task, 0};
    assert(sg_loading_v2_validate_round_witness(ctx, &round, &info, evidence) == SG_STATUS_OK);
    SGPlanRoute plan = {0, &task, 1};
    SGLoadingV2Report *validated = NULL;
    assert(sg_loading_v2_validate_plan(ctx, 1, &plan, NULL, SG_LOADING_V2_REPLAY_WITNESSES, &info,
                                       1, evidence, &validated) == SG_STATUS_OK);
    view(validated);
    // A forged summary cannot be replaced with the solver's alternative proof.
    SGLoadingV2RoundEvidence bad = *evidence;
    SGLoadingV2AxleState states[3];
    memcpy(states, evidence->axle_states, sizeof(states));
    states[1].front_min_g.numerator++;
    bad.axle_states = states;
    SGLoadingV2Report *rejected = (SGLoadingV2Report *)1;
    assert(sg_loading_v2_validate_plan(ctx, 1, &plan, NULL, SG_LOADING_V2_REPLAY_WITNESSES, &info,
                                       1, &bad, &rejected) == SG_STATUS_INVALID_ARG);
    assert(rejected == NULL && sg_get_unassigned(ctx) == 0);
    ShJsonBuf response;
    ShJsonWriter writer;
    sh_json_buf_init(&response);
    sh_json_writer_init(&writer, sh_json_buf_write, &response);
    assert(sg_api_write_solution(ctx, &writer, SG_STATUS_INVALID_ARG) == SG_STATUS_OK);
    assert(response.buf && strstr(response.buf, "invalid_witness") &&
           !strstr(response.buf, "\"routes\""));
    sh_json_buf_free(&response);
    SGLoadingV2Report *still = NULL;
    assert(sg_loading_v2_solution_report(ctx, &still) == SG_STATUS_OK);
    view(still);
    sg_loading_v2_report_free(still);
    // Fractional scoped departure must propagate without integer truncation.
    round.departure_sec = 0.5;
    SGLoadingV2Report *scoped = NULL;
    assert(sg_loading_v2_check_round(ctx, &round, NULL, NULL, &scoped) == SG_STATUS_OK);
    uint32_t count, dc;
    const SGLoadingV2RoundEvidence *rounds;
    const SGLoadingV2Diagnostic *diagnostics;
    assert(sg_loading_v2_report_view(scoped, &count, &rounds, &dc, &diagnostics) == SG_STATUS_OK);
    assert(count == 1 && rounds[0].outcome == SG_LOADING_FEASIBLE &&
           rounds[0].stops[0].departure_sec == 60.5);
    SGStats stats;
    assert(sg_loading_v2_report_stats(scoped, &stats) == SG_STATUS_ERROR);
    sg_loading_v2_report_free(scoped);
    assert(sg_loading_v2_check_round(ctx, &round, cancel_check, NULL, &scoped) == SG_STATUS_OK);
    assert(sg_loading_v2_report_view(scoped, &count, &rounds, &dc, &diagnostics) == SG_STATUS_OK);
    assert(rounds[0].outcome == SG_LOADING_UNKNOWN && rounds[0].operation_count == 0);
    sg_loading_v2_report_free(scoped);
    // Budget changes invalidate cached searches and saved snapshots, while a
    // caller-owned certificate can still be independently replayed at zero.
    SGLoadingV2Config zero = {sizeof(zero), 2, 3, 0, 0};
    assert(sg_loading_v2_set_config(ctx, &zero) == SG_STATUS_OK);
    SGLoadingV2Report *none = NULL;
    assert(sg_loading_v2_solution_report(ctx, &none) == SG_STATUS_ERROR && !none);
    SGLoadingV2ReportInfo zero_info = info;
    zero_info.node_budget = 0;
    SGLoadingV2RoundEvidence zero_witness = *evidence;
    zero_witness.nodes = 0;
    assert(sg_loading_v2_validate_plan(ctx, 1, &plan, NULL, SG_LOADING_V2_REPLAY_WITNESSES,
                                       &zero_info, 1, &zero_witness, &none) == SG_STATUS_OK);
    view(none);
    sg_loading_v2_report_free(none);
    assert(sg_loading_v2_reset(ctx) == SG_STATUS_OK);
    assert(sg_set_hard_loading(ctx, 0, 0) == SG_STATUS_OK);
    sg_free(ctx); // owned reports remain valid after context destruction
    view(report);
    view(validated);
    sg_loading_v2_report_free(report);
    sg_loading_v2_report_free(validated);
    puts("loading v2 C: HGS, owned inputs/reports, exact replay, staged "
         "rejection, scoped schedule and cancellation passed");
    return 0;
}
