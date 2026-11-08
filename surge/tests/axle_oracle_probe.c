/* Test-only flat bridge for Python Fraction oracle. Never linked into libsurge. */
#include "../src/sg_axle.h"
#include <string.h>
int sg_axle_oracle_probe(const int64_t *in, uint32_t length, int64_t *out) {
    if (!in || !out || length < 18 || in[17] < 0 || in[17] > 128 ||
        length != 18 + 4*(uint32_t)in[17]) return SG_STATUS_INVALID_ARG;
    SGLoadingVehicle v={0};v.slot_count=1;
    SGAxleProfile p={0};
    p.wheelbase_mm=(uint32_t)in[0];p.deck_min_mm=(int32_t)in[1];p.deck_max_mm=(int32_t)in[2];
    p.footprint_length_mm=(uint32_t)in[3];p.slot_centres_mm[0]=(int32_t)in[4];p.slot_count=1;
    p.front_tare_g=(SGAxleMassInterval){(uint64_t)in[5],(uint64_t)in[6]};
    p.rear_tare_g=(SGAxleMassInterval){(uint64_t)in[7],(uint64_t)in[8]};
    p.front_limits_g=(SGAxleMassInterval){(uint64_t)in[9],(uint64_t)in[10]};
    p.rear_limits_g=(SGAxleMassInterval){(uint64_t)in[11],(uint64_t)in[12]};
    p.max_gross_g=(uint64_t)in[13];p.has_min_front_fraction=(uint8_t)in[14];
    p.min_front_fraction=(SGAxleFraction){(uint32_t)in[15],(uint32_t)in[16]};
    uint32_t n=(uint32_t)in[17],slots[128]={0};
    SGLoadingUnit units[128]={{0}};SGAxleSignedInterval cg[128];uint8_t active[128];
    for(uint32_t i=0;i<n;i++){
        units[i].weight_g=(uint64_t)in[18+4*i];
        cg[i]=(SGAxleSignedInterval){(int32_t)in[19+4*i],(int32_t)in[20+4*i]};
        active[i]=(uint8_t)in[21+4*i];
    }
    SGAxleState r;SGStatus status=sg_axle_state(&v,&p,units,cg,slots,active,n,&r);
    out[0]=r.front_min_num;out[1]=r.front_max_num;out[2]=r.rear_min_num;out[3]=r.rear_max_num;
    out[4]=r.denominator;out[5]=(int64_t)r.gross_min_g;out[6]=(int64_t)r.gross_max_g;
    return status;
}

int sg_axle_joint_probe(const int64_t *in, uint32_t length, int64_t *out) {
    if (!in || !out || length<22 || in[17]<0 || in[17]>3 ||
        length!=22+6*(uint32_t)in[17]) return SG_STATUS_INVALID_ARG;
    SGLoadingVehicle v={0};v.slot_count=2;v.max_layers=2;v.clearance_mm=2400;
    v.class_count=2;v.max_stack_weight_g=12000000;v.payload_g=12000000;
    v.access=(SGLoadingAccess)in[19];v.allowed_top[0]=(uint64_t)in[20];v.allowed_top[1]=(uint64_t)in[21];
    if(v.access==SG_LOADING_REAR_LIFO){v.depth[0]=1;v.depth[1]=0;}else v.lane[1]=1;
    SGAxleProfile p={0};p.slot_count=2;
    p.wheelbase_mm=(uint32_t)in[0];p.deck_min_mm=(int32_t)in[1];p.deck_max_mm=(int32_t)in[2];
    p.footprint_length_mm=(uint32_t)in[3];p.slot_centres_mm[0]=(int32_t)in[4];p.slot_centres_mm[1]=(int32_t)in[18];
    p.front_tare_g=(SGAxleMassInterval){(uint64_t)in[5],(uint64_t)in[6]};
    p.rear_tare_g=(SGAxleMassInterval){(uint64_t)in[7],(uint64_t)in[8]};
    p.front_limits_g=(SGAxleMassInterval){(uint64_t)in[9],(uint64_t)in[10]};
    p.rear_limits_g=(SGAxleMassInterval){(uint64_t)in[11],(uint64_t)in[12]};
    p.max_gross_g=(uint64_t)in[13];p.has_min_front_fraction=(uint8_t)in[14];
    p.min_front_fraction=(SGAxleFraction){(uint32_t)in[15],(uint32_t)in[16]};
    uint32_t n=(uint32_t)in[17];SGLoadingUnit units[3]={{0}};SGAxleSignedInterval cg[3]={{0,0}};
    for(uint32_t i=0;i<n;i++){
        units[i].weight_g=(uint64_t)in[22+6*i];units[i].stop=(uint32_t)in[23+6*i];
        units[i].stack_class=(uint32_t)in[24+6*i];units[i].max_units_above=(uint32_t)in[25+6*i];
        units[i].max_weight_above_g=(uint64_t)in[26+6*i];units[i].height_mm=(uint32_t)in[27+6*i];
    }
    SGAxleLoadingResult r;SGStatus status=sg_axle_loading_check(&v,&p,units,cg,n,100000,NULL,NULL,&r);
    out[0]=r.loading.outcome;out[1]=(int64_t)r.loading.nodes;
    for(uint32_t i=0;i<n;i++){out[2+2*i]=r.loading.placements[i].slot;out[3+2*i]=r.loading.placements[i].layer;out[2+2*n+i]=r.unload_order[i];}
    return status;
}
