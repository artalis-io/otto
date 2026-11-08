#define main loading_v2_other_main
#include "test_loading_v2.c"
#undef main
#include "sh_pal.h"
#include <stdatomic.h>

typedef struct {SGContext *ctx;uint64_t requested;} CancelTest;
static void *request_cancel(void *data) {
    CancelTest *c=data;
    sh_sleep_ms(20);
    c->requested=sh_monotonic_ms();
    assert(sg_cancel(c->ctx)==SG_STATUS_OK);
    return NULL;
}
int main(void) {
    SGContext *ctx=base();configure(ctx);
    SGLoadingV2SignedInterval cg={-50,50};
    SGLoadingV2Unit unit={0};unit.physical.height_mm=1000;
    unit.physical.weight_g=500000;unit.physical.max_weight_above_g=500000;
    unit.physical.max_units_above=1;unit.cg_offset_mm=&cg;
    for(uint32_t i=1;i<24;i++) {
        assert(sg_add_delivery_request(ctx,(i*19)%53,(i*7)%31,0,100000,0,-1)==i);
        assert(sg_loading_v2_set_request(ctx,i,&unit,1)==SG_STATUS_OK);
    }
    SGConfig cfg;sg_config_default(&cfg);cfg.max_iterations=1000000;cfg.max_time_seconds=0;cfg.seed=42;
    assert(sg_set_config(ctx,&cfg)==SG_STATUS_OK);
    CancelTest c={ctx,0};ShThread thread;
    assert(sh_thread_create(&thread,request_cancel,&c)==0);
    SGPopulationConfig pop={2,4,3,0.5};
    SGStatus status=sg_solve_population(ctx,&pop);
    uint64_t returned=sh_monotonic_ms();
    assert(sh_thread_join(&thread,NULL)==0);
    assert(c.requested && (status==SG_STATUS_LIMIT||status==SG_STATUS_ERROR));
    SGLoadingV2Report *r=NULL;
    assert(sg_loading_v2_solution_report(ctx,&r)!=SG_STATUS_OK && !r);
    printf("loading v2 cancellation: native HGS returned %llu ms after request; no publishable report\n",(unsigned long long)(returned>=c.requested?returned-c.requested:0));
    sg_free(ctx);return 0;
}
