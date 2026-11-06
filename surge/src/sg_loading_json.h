#ifndef SG_LOADING_JSON_H
#define SG_LOADING_JSON_H
#include "surge.h"
#include "sh_json.h"
SGStatus sg_loading_build_json(SGContext *ctx, const ShJsonValue *value);
void sg_loading_write_json(const SGContext *ctx, ShJsonWriter *writer);
#endif
