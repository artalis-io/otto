#include "sg_loading.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#ifndef _WIN32
#include <pthread.h>
#endif
static SGLoadingVehicle vehicle(unsigned slots) {
    SGLoadingVehicle v = {0};
    v.slot_count=slots; v.max_layers=2; v.clearance_mm=2400;
    v.max_stack_weight_g=2000; v.payload_g=10000;
    v.class_count=2; v.allowed_top[0]=3; v.allowed_top[1]=0;
    for(unsigned i=0;i<slots;i++)v.depth[i]=i;
    return v;
}
static SGLoadingUnit unit(unsigned stop,unsigned cls) {
    return (SGLoadingUnit){stop,1000,500,1000,cls,1};
}
static SGLoadingResult run(SGLoadingVehicle *v,SGLoadingUnit *u,unsigned n,uint64_t budget) {
    SGLoadingResult r; assert(sg_loading_check(v,u,n,budget,&r)==SG_STATUS_OK);
    if(r.outcome==SG_LOADING_FEASIBLE)assert(sg_loading_validate(v,u,n,r.placements)==SG_STATUS_OK);
    else assert(r.count==0);
    return r;
}
static void examples(void) {
    SGLoadingVehicle v=vehicle(1); SGLoadingUnit u[4]={unit(1,1),unit(2,0),unit(1,0),unit(2,0)};
    SGLoadingResult r=run(&v,u,2,1000);
    assert(r.outcome==SG_LOADING_FEASIBLE && r.placements[1].layer==0); /* input order != stack order */
    v.access=SG_LOADING_REAR_LIFO;
    assert(run(&v,u,2,1000).outcome==SG_LOADING_FEASIBLE);
    u[0].stop=3;
    assert(run(&v,u,2,1000).outcome==SG_LOADING_INFEASIBLE); /* class forces inaccessible top */
    assert(run(&v,u,2,1).outcome==SG_LOADING_UNKNOWN);
    v.access=SG_LOADING_SIDE_FREE;u[0].height_mm=1500;
    assert(run(&v,u,2,1000).outcome==SG_LOADING_INFEASIBLE);
    u[0]=unit(1,1);u[1].max_weight_above_g=499;
    assert(run(&v,u,2,1000).outcome==SG_LOADING_INFEASIBLE);
    u[1]=unit(2,0);v.payload_g=999;
    assert(run(&v,u,2,1000).reason==SG_LOADING_CAPACITY);
    v=vehicle(2);v.max_layers=1;v.access=SG_LOADING_REAR_LIFO;
    SGLoadingPlacement p[2]={{0,0},{1,0}};u[0]=unit(2,0);u[1]=unit(1,0);
    assert(sg_loading_validate(&v,u,2,p)==SG_STATUS_INFEASIBLE);
    assert(run(&v,u,2,1000).outcome==SG_LOADING_FEASIBLE);
    v.lane[1]=1;assert(sg_loading_validate(&v,u,2,p)==SG_STATUS_OK);
    p[1]=p[0];assert(sg_loading_validate(&v,u,2,p)==SG_STATUS_INFEASIBLE);
    p[1]=(SGLoadingPlacement){1,1};assert(sg_loading_validate(&v,u,2,p)==SG_STATUS_INFEASIBLE);
    v=vehicle(1);v.max_layers=3;u[0]=unit(3,0);u[1]=unit(2,0);u[2]=unit(1,0);
    v.clearance_mm=4000;u[0].max_units_above=2;u[0].max_weight_above_g=999;
    SGLoadingPlacement three[3]={{0,0},{0,1},{0,2}};
    assert(sg_loading_validate(&v,u,3,three)==SG_STATUS_INFEASIBLE); /* all weight above, not just neighbor */
    v=vehicle(1);u[0]=unit(1,0);u[1]=unit(1,0);
    u[0].max_units_above=0;u[1].max_units_above=0;
    assert(run(&v,u,2,1000).outcome==SG_LOADING_INFEASIBLE);
    v=vehicle(1);u[0]=unit(1,0);u[0].weight_g=UINT64_MAX;
    assert(run(&v,u,1,100).outcome==SG_LOADING_INFEASIBLE);
    u[0]=unit(1,0);v.class_count=64;v.allowed_top[0]=UINT64_C(1)<<63;u[1]=unit(1,63);
    assert(run(&v,u,2,1000).outcome==SG_LOADING_FEASIBLE);
    assert(run(&v,NULL,0,1).outcome==SG_LOADING_FEASIBLE);
    assert(run(&v,u,1,0).outcome==SG_LOADING_UNKNOWN);
    v.slot_count=129;assert(sg_loading_check(&v,u,1,1,&r)==SG_STATUS_INVALID_ARG && r.outcome==SG_LOADING_UNKNOWN);
    v=vehicle(1);u[0].stack_class=2;assert(sg_loading_check(&v,u,1,1,&r)==SG_STATUS_INVALID_ARG);
    assert(sg_loading_check(NULL,NULL,0,1,&r)==SG_STATUS_INVALID_ARG);
}
/* Independent brute-force placement oracle for 3 units/2 slots/2 layers.
 * Enumerates certificates, not the search's placement sequence. */
static int oracle(SGLoadingVehicle *v,SGLoadingUnit *u,SGLoadingPlacement *p,unsigned at) {
    if(at==3)return sg_loading_validate(v,u,3,p)==SG_STATUS_OK;
    for(unsigned slot=0;slot<2;slot++)for(unsigned layer=0;layer<2;layer++){
        p[at]=(SGLoadingPlacement){slot,layer};
        if(oracle(v,u,p,at+1))return 1;
    }
    return 0;
}
static void exhaustive(void) {
    unsigned cases=0;
    for(unsigned access=0;access<2;access++)for(unsigned rules=0;rules<16;rules++)
    for(unsigned bits=0;bits<64;bits++){
        SGLoadingVehicle v=vehicle(2);v.access=(SGLoadingAccess)access;
        v.allowed_top[0]=rules&3;v.allowed_top[1]=(rules>>2)&3;
        SGLoadingUnit u[3];for(unsigned i=0;i<3;i++)u[i]=unit((bits>>(i+3))&1,(bits>>i)&1);
        SGLoadingPlacement p[3];int expected=oracle(&v,u,p,0);
        SGLoadingResult r=run(&v,u,3,100000);
        assert(r.outcome!=SG_LOADING_UNKNOWN);
        assert((r.outcome==SG_LOADING_FEASIBLE)==expected);
        SGLoadingResult again=run(&v,u,3,100000);assert(memcmp(&r,&again,sizeof(r))==0);
        cases++;
    }
    printf("loading: %u exhaustive oracle comparisons passed\n",cases);
}
static void varied_geometry(void) {
    unsigned seed=173;
    for(unsigned trial=0;trial<512;trial++) {
        SGLoadingVehicle v=vehicle(2);v.access=(SGLoadingAccess)(trial%2);
        v.max_stack_weight_g=1300;v.payload_g=1800;v.clearance_mm=1800;
        SGLoadingUnit u[3];
        for(unsigned i=0;i<3;i++) {
            seed=seed*1664525u+1013904223u;u[i]=unit((seed>>8)%3,seed%2);
            u[i].height_mm=100+(seed>>4)%1400;u[i].weight_g=(seed>>12)%900;
            u[i].max_weight_above_g=(seed>>15)%1000;u[i].max_units_above=(seed>>20)%3;
        }
        SGLoadingPlacement p[3];int expected=oracle(&v,u,p,0);
        SGLoadingResult r=run(&v,u,3,100000);
        assert(r.outcome!=SG_LOADING_UNKNOWN);
        assert((r.outcome==SG_LOADING_FEASIBLE)==expected);
    }
    puts("loading: 512 varied geometry/weight oracle comparisons passed");
}
#ifndef _WIN32
static void *thread_check(void *unused) {
    (void)unused;
    SGLoadingVehicle v=vehicle(2);SGLoadingUnit u[3]={unit(2,0),unit(1,1),unit(1,0)};
    for(unsigned i=0;i<100;i++)assert(run(&v,u,3,1000).outcome==SG_LOADING_FEASIBLE);
    return NULL;
}
static void concurrent(void) {
    pthread_t threads[8];
    for(unsigned i=0;i<8;i++)assert(pthread_create(&threads[i],NULL,thread_check,NULL)==0);
    for(unsigned i=0;i<8;i++)assert(pthread_join(threads[i],NULL)==0);
}
#else
static void concurrent(void) { /* POSIX concurrency exercised on Linux/macOS. */ }
#endif
static void full_load(void) {
    SGLoadingVehicle v=vehicle(33);v.payload_g=100000;
    SGLoadingUnit u[66];for(unsigned i=0;i<66;i++)u[i]=unit(1,0);
    SGLoadingResult r=run(&v,u,66,1000);assert(r.outcome==SG_LOADING_FEASIBLE);
    SGLoadingPlacement damaged[66];memcpy(damaged,r.placements,sizeof(damaged));
    damaged[0].layer=7;assert(sg_loading_validate(&v,u,66,damaged)==SG_STATUS_INFEASIBLE);
    v.clearance_mm=1900;assert(run(&v,u,66,200).outcome!=SG_LOADING_FEASIBLE);
}
#include "loading_oracle.h"

int main(void) { examples();exhaustive();varied_geometry();randomized_loading_oracle();full_load();concurrent();puts("loading: examples passed");return 0; }
