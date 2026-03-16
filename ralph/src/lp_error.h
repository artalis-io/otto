#ifndef LP_ERROR_H
#define LP_ERROR_H

#include "ralph_core.h"

typedef struct {
    RalphAPIError error;
    int valid;
} LPAPIErrorState;

void lp_error_state_init(LPAPIErrorState *state);
void lp_error_state_clear(LPAPIErrorState *state);
int lp_error_state_get(const LPAPIErrorState *state, RalphAPIError *out);
void lp_error_state_set(LPAPIErrorState *state,
                        RalphErrorDomain domain,
                        RalphErrorCode code,
                        RalphStatus status_hint,
                        RalphErrorAPIId api_id,
                        int detail_i0,
                        int detail_i1,
                        const char *message);

void lp_error_tls_clear(void);
int lp_error_tls_get(RalphAPIError *out);
void lp_error_tls_set(RalphErrorDomain domain,
                      RalphErrorCode code,
                      RalphStatus status_hint,
                      RalphErrorAPIId api_id,
                      int detail_i0,
                      int detail_i1,
                      const char *message);

const char* lp_error_domain_string(RalphErrorDomain domain);
const char* lp_error_code_string(RalphErrorCode code);
const char* lp_error_api_string(RalphErrorAPIId api_id);
const char* lp_error_message(const RalphAPIError *error);

#endif /* LP_ERROR_H */
