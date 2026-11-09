#include "../src/sg_axle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static SGLoadingVehicle vehicle(void) {
    SGLoadingVehicle v = {0};
    v.slot_count = 2; v.lane[1] = 1;
    v.max_layers = 2; v.clearance_mm = 2400; v.class_count = 1;
    v.max_stack_weight_g = 10000000; v.payload_g = 10000000;
    v.allowed_top[0] = 1;
    return v;
}
static SGAxleProfile profile(void) {
    SGAxleProfile p = {0};
    p.wheelbase_mm = 4000; p.footprint_length_mm = 1200; p.slot_count = 2;
    p.deck_min_mm = -3000; p.deck_max_mm = 6000;
    p.slot_centres_mm[0] = -2000; p.slot_centres_mm[1] = 5000;
    p.front_tare_g = (SGAxleMassInterval){3000000,3000000};
    p.rear_tare_g = (SGAxleMassInterval){2000000,2000000};
    p.front_limits_g = (SGAxleMassInterval){0,6000000};
    p.rear_limits_g = (SGAxleMassInterval){0,6500000};
    p.max_gross_g = 12000000;
    return p;
}
static void counterweight(void) {
    SGLoadingVehicle v = vehicle(); SGAxleProfile p = profile();
    SGLoadingUnit u[2] = {{0}}; u[0].weight_g = 1000000; u[1].weight_g = 4000000;
    SGAxleSignedInterval cg[2] = {{0,0},{0,0}};
    uint32_t slots[2] = {0,1}; uint8_t active[2] = {1,1}; SGAxleState r;
    assert(sg_axle_state(&v,&p,u,cg,slots,active,2,&r)==SG_STATUS_OK);
    assert(r.front_min_num==INT64_C(3500000)*4000);
    assert(r.rear_max_num==INT64_C(6500000)*4000);
    active[0]=0;
    assert(sg_axle_state(&v,&p,u,cg,slots,active,2,&r)==SG_STATUS_INFEASIBLE);
    assert(r.rear_max_num==INT64_C(7000000)*4000);
    active[0]=1; active[1]=0;
    assert(sg_axle_state(&v,&p,u,cg,slots,active,2,&r)==SG_STATUS_OK);
    assert(r.front_min_num==INT64_C(4500000)*4000);
    active[0]=0;
    assert(sg_axle_state(&v,&p,u,cg,slots,active,2,&r)==SG_STATUS_OK);
}
static void uncertainty_and_fraction(void) {
    SGLoadingVehicle v = vehicle(); SGAxleProfile p = profile();
    p.front_tare_g=(SGAxleMassInterval){2900000,3100000};
    p.rear_tare_g=(SGAxleMassInterval){1900000,2100000};
    p.has_min_front_fraction=1; p.min_front_fraction=(SGAxleFraction){58,100};
    SGAxleState r;
    assert(sg_axle_state(&v,&p,NULL,NULL,NULL,NULL,0,&r)==SG_STATUS_OK);
    p.min_front_fraction=(SGAxleFraction){580001,1000000};
    assert(sg_axle_state(&v,&p,NULL,NULL,NULL,NULL,0,&r)==SG_STATUS_INFEASIBLE);
    p=profile(); p.slot_centres_mm[0]=2000;
    SGLoadingUnit u={0};u.weight_g=500000;
    SGAxleSignedInterval cg={-50,50};uint32_t slot=0;uint8_t active=1;
    assert(sg_axle_state(&v,&p,&u,&cg,&slot,&active,1,&r)==SG_STATUS_OK);
    assert(r.front_min_num==INT64_C(12975000000));
    assert(r.front_max_num==INT64_C(13025000000));
    assert(r.rear_min_num==INT64_C(8975000000));
    assert(r.rear_max_num==INT64_C(9025000000));
    p.front_limits_g.max=3243750;
    assert(sg_axle_state(&v,&p,&u,&cg,&slot,&active,1,&r)==SG_STATUS_INFEASIBLE);
    p=profile(); p.front_tare_g=(SGAxleMassInterval){0,0};p.rear_tare_g=p.front_tare_g;
    p.has_min_front_fraction=1;p.min_front_fraction=(SGAxleFraction){1,1};
    assert(sg_axle_state(&v,&p,NULL,NULL,NULL,NULL,0,&r)==SG_STATUS_OK);
}
static void malformed(void) {
    SGLoadingVehicle v=vehicle();SGAxleProfile p=profile();SGAxleState r;
    SGAxleProfile bad=p;bad.wheelbase_mm=0;
    memset(&r,0xaa,sizeof(r));
    assert(sg_axle_state(&v,&bad,NULL,NULL,NULL,NULL,0,&r)==SG_STATUS_INVALID_ARG);
    const unsigned char *bytes=(const unsigned char *)&r;
    for (size_t i=0;i<sizeof(r);i++)assert(bytes[i]==0);
    bad=p;bad.front_tare_g.min=bad.front_tare_g.max+1;
    assert(sg_axle_profile_validate(&v,&bad)==SG_STATUS_INVALID_ARG);
    bad=p;bad.has_min_front_fraction=1;bad.min_front_fraction=(SGAxleFraction){2,1};
    assert(sg_axle_profile_validate(&v,&bad)==SG_STATUS_INVALID_ARG);
    bad=p;bad.min_front_fraction.denominator=1;
    assert(sg_axle_profile_validate(&v,&bad)==SG_STATUS_INVALID_ARG);
    bad=p;bad.slot_count=1;assert(sg_axle_profile_validate(&v,&bad)==SG_STATUS_INVALID_ARG);
    bad=p;bad.slot_centres_mm[0]=bad.deck_min_mm;
    assert(sg_axle_profile_validate(&v,&bad)==SG_STATUS_INVALID_ARG);
    SGLoadingUnit u={0};u.weight_g=1;SGAxleSignedInterval cg={-601,0};
    uint32_t slot=0;uint8_t active=0;
    assert(sg_axle_state(&v,&p,&u,&cg,&slot,&active,1,&r)==SG_STATUS_INVALID_ARG);
    cg=(SGAxleSignedInterval){0,0};active=2;
    assert(sg_axle_state(&v,&p,&u,&cg,&slot,&active,1,&r)==SG_STATUS_INVALID_ARG);
    active=1;u.weight_g=SG_AXLE_MAX_MASS_G+1;
    assert(sg_axle_state(&v,&p,&u,&cg,&slot,&active,1,&r)==SG_STATUS_INVALID_ARG);
    assert(sg_axle_state(&v,&p,NULL,NULL,NULL,NULL,129,&r)==SG_STATUS_INVALID_ARG);
    assert(sg_axle_state(&v,&p,NULL,NULL,NULL,NULL,0,NULL)==SG_STATUS_INVALID_ARG);
    v.lane[1]=0;v.depth[0]=0;v.depth[1]=1;v.access=SG_LOADING_REAR_LIFO;
    assert(sg_axle_profile_validate(&v,&p)==SG_STATUS_INVALID_ARG);
    v.depth[0]=1;v.depth[1]=0;
    assert(sg_axle_profile_validate(&v,&p)==SG_STATUS_OK);
    p.slot_centres_mm[1]=-1000;
    assert(sg_axle_profile_validate(&v,&p)==SG_STATUS_INVALID_ARG);
}
static void extremes(void) {
    SGLoadingVehicle v=vehicle();v.slot_count=1;
    SGAxleProfile p=profile();p.slot_count=1;p.wheelbase_mm=1000000;
    p.deck_min_mm=-1000000;p.deck_max_mm=1000000;
    p.footprint_length_mm=1000;p.slot_centres_mm[0]=-999500;
    p.front_tare_g=(SGAxleMassInterval){0,0};p.rear_tare_g=p.front_tare_g;
    SGLoadingUnit u[128]={{0}};SGAxleSignedInterval cg[128];uint32_t slots[128]={0};uint8_t active[128];
    for(unsigned i=0;i<128;i++){u[i].weight_g=1000000000;cg[i]=(SGAxleSignedInterval){-500,500};active[i]=1;}
    SGAxleState r;
    assert(sg_axle_state(&v,&p,u,cg,slots,active,128,&r)==SG_STATUS_INFEASIBLE);
    assert(r.front_max_num==INT64_C(256000000000000000));
    assert(r.rear_min_num==-INT64_C(128000000000000000));
    assert(r.gross_max_g==UINT64_C(128000000000));
}
static int cancel_now(void *data) { (void)data;return 1; }
static void joint_search(void) {
    SGLoadingVehicle v=vehicle();v.max_layers=1;
    SGAxleProfile p=profile();
    SGLoadingUnit u[2]={{0}};
    for(unsigned i=0;i<2;i++){u[i].height_mm=1000;u[i].max_units_above=0;}
    u[0].weight_g=1000000;u[1].weight_g=4000000;u[1].stop=1;
    SGAxleSignedInterval cg[2]={{0,0},{0,0}};SGAxleLoadingResult r;
    assert(sg_axle_loading_check(&v,&p,u,cg,2,10000,NULL,NULL,&r)==SG_STATUS_OK);
    assert(r.loading.outcome==SG_LOADING_INFEASIBLE);
    assert(r.loading.count==0);
    // Same arrangement, same stop: the valid removal order is B before A.
    u[1].stop=0;
    assert(sg_axle_loading_check(&v,&p,u,cg,2,10000,NULL,NULL,&r)==SG_STATUS_OK);
    assert(r.loading.outcome==SG_LOADING_FEASIBLE && r.unload_order[0]==1);
    assert(sg_axle_loading_validate(&v,&p,u,cg,2,r.loading.placements,r.unload_order,NULL)==SG_STATUS_OK);
    uint32_t wrong[2]={0,1};
    assert(sg_axle_loading_validate(&v,&p,u,cg,2,r.loading.placements,wrong,NULL)==SG_STATUS_INFEASIBLE);
    wrong[0]=1;wrong[1]=1;
    assert(sg_axle_loading_validate(&v,&p,u,cg,2,r.loading.placements,wrong,NULL)==SG_STATUS_INFEASIBLE);
    // First stack-feasible arrangement fails, but a different slot succeeds.
    u[1].stop=1;v.slot_count=3;v.lane[2]=2;p.slot_count=3;
    p.slot_centres_mm[2]=2000;p.front_limits_g.max=7000000;
    assert(sg_axle_loading_check(&v,&p,u,cg,2,10000,NULL,NULL,&r)==SG_STATUS_OK);
    assert(r.loading.outcome==SG_LOADING_FEASIBLE && r.loading.placements[1].slot==2);
    assert(sg_axle_loading_validate(&v,&p,u,cg,2,r.loading.placements,r.unload_order,NULL)==SG_STATUS_OK);
    assert(sg_axle_loading_check(&v,&p,u,cg,2,3,NULL,NULL,&r)==SG_STATUS_OK);
    assert(r.loading.outcome==SG_LOADING_UNKNOWN && r.loading.count==0 && r.loading.nodes<=3);
    assert(sg_axle_loading_check(&v,&p,u,cg,2,10000,cancel_now,NULL,&r)==SG_STATUS_OK);
    assert(r.loading.outcome==SG_LOADING_UNKNOWN && r.loading.reason==SG_LOADING_INTERRUPTED);
    // V2 zero payload/stack limits remain literal; V1 retains its old rejection.
    v=vehicle();v.slot_count=1;v.payload_g=0;v.max_stack_weight_g=0;
    p=profile();p.slot_count=1;u[0].weight_g=0;u[0].stop=0;
    assert(sg_axle_loading_check(&v,&p,u,cg,1,100,NULL,NULL,&r)==SG_STATUS_OK);
    assert(r.loading.outcome==SG_LOADING_FEASIBLE);
    SGLoadingResult legacy;
    assert(sg_loading_check(&v,u,1,100,&legacy)==SG_STATUS_INVALID_ARG);
    u[0].height_mm=0;
    assert(sg_axle_loading_check(&v,&p,u,cg,1,100,NULL,NULL,&r)==SG_STATUS_INVALID_ARG);
}
int main(void) {
    counterweight();uncertainty_and_fraction();malformed();extremes();joint_search();
    puts("axle kernel: counterweight, uncertainty, exact fractions, geometry and numeric boundaries passed");
    return 0;
}
