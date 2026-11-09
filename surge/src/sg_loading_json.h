#ifndef SG_LOADING_JSON_H
#define SG_LOADING_JSON_H
#include "surge.h"
#include "sh_json.h"
SGStatus sg_loading_build_json(SGContext *ctx, const ShJsonValue *value);
void sg_loading_write_json(const SGContext *ctx, ShJsonWriter *writer);
SGStatus sg_loading_v2_build_json(SGContext *ctx, const ShJsonValue *value);
void sg_loading_v2_write_report(ShJsonWriter *writer, const SGLoadingV2Report *report);
void sg_loading_v2_write_capabilities(ShJsonWriter *writer);
SGStatus sg_loading_v2_validate_json(SGContext *ctx, const ShJsonValue *value, uint32_t count, const SGPlanRoute *routes, const uint8_t *const *flags);
void sg_loading_v2_write_error(const SGContext *ctx, ShJsonWriter *writer, SGStatus status);
int sg_loading_v2_plan_json_shape(const SGContext *ctx, const ShJsonValue *value);
#endif
