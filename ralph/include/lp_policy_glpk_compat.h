#ifndef LP_POLICY_GLPK_COMPAT_H
#define LP_POLICY_GLPK_COMPAT_H

/* GLPK-like policy profile selector. */
typedef enum {
    LP_POLICY_PROFILE_DEFAULT = 0,
    LP_POLICY_PROFILE_GLPK_COMPAT = 1,
    LP_POLICY_PROFILE_GLPK_STRICT = 2,
    LP_POLICY_PROFILE_GLPK_LEGACY = 3
} LPPolicyProfile;

/* SMCP-like controls. */
typedef enum {
    LP_GLPK_SMCP_METHOD_AUTO = 0,
    LP_GLPK_SMCP_METHOD_PRIMAL = 1,
    LP_GLPK_SMCP_METHOD_DUALP = 2,
    LP_GLPK_SMCP_METHOD_DUAL = 3
} LPGLPKSMCPMethod;

typedef enum {
    LP_GLPK_SMCP_PRICING_STANDARD = 0,
    LP_GLPK_SMCP_PRICING_STEEP = 1
} LPGLPKSMCPPricing;

typedef enum {
    LP_GLPK_SMCP_RATIO_STANDARD = 0,
    LP_GLPK_SMCP_RATIO_HARRIS = 1
} LPGLPKSMCPRatio;

typedef enum {
    LP_GLPK_SMCP_FLIP_OFF = 0,
    LP_GLPK_SMCP_FLIP_ON = 1
} LPGLPKSMCPFlip;

typedef enum {
    LP_GLPK_SMCP_BASIS_ADV = 0,
    LP_GLPK_SMCP_BASIS_STD = 1,
    LP_GLPK_SMCP_BASIS_BIB = 2,
    LP_GLPK_SMCP_BASIS_INI = 3
} LPGLPKSMCPBasis;

typedef enum {
    LP_GLPK_SMCP_PRESOLVE_AUTO = 0,
    LP_GLPK_SMCP_PRESOLVE_OFF = 1,
    LP_GLPK_SMCP_PRESOLVE_ON = 2
} LPGLPKSMCPPresolve;

typedef enum {
    LP_GLPK_SMCP_EXCL_OFF = 0,
    LP_GLPK_SMCP_EXCL_ON = 1
} LPGLPKSMCPExcl;

typedef enum {
    LP_GLPK_SMCP_SHIFT_OFF = 0,
    LP_GLPK_SMCP_SHIFT_ON = 1
} LPGLPKSMCPShift;

typedef enum {
    LP_GLPK_SMCP_AORN_USE_AT = 1,
    LP_GLPK_SMCP_AORN_USE_NT = 2
} LPGLPKSMCPAorn;

typedef enum {
    LP_GLPK_PERTURB_STATE_OFF = 0,
    LP_GLPK_PERTURB_STATE_ACTIVE = 1,
    LP_GLPK_PERTURB_STATE_CLEANUP = 2
} LPGLPKPerturbState;

typedef enum {
    LP_GLPK_PERTURB_EVENT_ENABLE = 0,
    LP_GLPK_PERTURB_EVENT_DISABLE = 1,
    LP_GLPK_PERTURB_EVENT_BEGIN_CLEANUP = 2,
    LP_GLPK_PERTURB_EVENT_REAPPLY = 3
} LPGLPKPerturbEvent;

/* BFCP-like controls. */
typedef enum {
    LP_GLPK_BFCP_BACKEND_LUF_FT = 0,
    LP_GLPK_BFCP_BACKEND_CBG = 1,
    LP_GLPK_BFCP_BACKEND_CGR = 2
} LPGLPKBFCPBackend;

typedef struct {
    int lp_policy_profile;
    int glpk_smcp_method;
    int glpk_smcp_pricing;
    int glpk_smcp_ratio;
    int glpk_smcp_flip;
    int glpk_smcp_basis;
    int glpk_smcp_presolve;
    double glpk_smcp_tol_bnd;      /* >0 */
    double glpk_smcp_tol_dj;       /* >0 */
    double glpk_smcp_tol_piv;      /* >0 */
    int glpk_smcp_excl;            /* 0=off, 1=on */
    int glpk_smcp_shift;           /* 0=off, 1=on */
    int glpk_smcp_aorn;            /* 1=use A^T, 2=use N^T */
    int glpk_bfcp_backend;
    int glpk_bfcp_update_limit;   /* -1 = auto */
    double glpk_bfcp_pivot_tol;   /* <=0 = auto */
    double glpk_bfcp_growth_guard;/* <=0 = auto */
} LPGLPKCompatConfig;

void lp_policy_glpk_compat_init(LPGLPKCompatConfig *cfg);
int lp_policy_glpk_compat_validate(const LPGLPKCompatConfig *cfg);
void lp_policy_glpk_compat_apply_profile_defaults(LPGLPKCompatConfig *cfg);

/* Map policy controls onto currently available Ralph runtime hooks. */
void lp_policy_glpk_compat_apply_runtime(const LPGLPKCompatConfig *cfg,
                                         int *method_io,
                                         int *pricing_io,
                                         int *phase1_pricing_io,
                                         int *presolve_io,
                                         int *ratio_io,
                                         int *dual_ratio_io,
                                         int *dual_bound_flip_io,
                                         double *smcp_tol_bnd_io,
                                         double *smcp_tol_dj_io,
                                         double *smcp_tol_piv_io,
                                         int *smcp_excl_io,
                                         int *smcp_shift_io,
                                         int *smcp_aorn_io,
                                         int *crash_io,
                                         int *bfcp_backend_io,
                                         int *bfcp_backend_supported_io,
                                         int *bfcp_update_limit_io,
                                         double *bfcp_pivot_tol_io,
                                         double *bfcp_growth_guard_io,
                                         int *dual_refactor_base_interval_io,
                                         int *dual_rc_recompute_interval_io,
                                         int *soft_lu_cost_gate_enabled_io,
                                         int *periodic_cost_gate_enabled_io);

/* Working-LP semantics helpers (orthogonal policy surface):
 * - excl/shift: classify boxed non-basic columns removable from working LP.
 * - aorn: select A^T (row-kernel) vs N^T (column-kernel) ratio evaluation. */
int lp_policy_glpk_working_exclude_nonbasic(int smcp_excl,
                                            int smcp_shift,
                                            int var_status,
                                            double lb,
                                            double ub,
                                            double tol_bnd);
int lp_policy_glpk_working_use_at_kernel(int smcp_aorn, int has_row_scatter);
int lp_policy_glpk_perturb_next_state(int state, int event, int *next_state_out);
int lp_policy_glpk_basis_crash_mode(int smcp_basis, int *crash_mode_out);
int lp_policy_glpk_basis_requires_staged_basis(int smcp_basis);
int lp_policy_glpk_basis_supports_current_runtime(int smcp_basis);
const char* lp_policy_glpk_basis_name(int smcp_basis);

#endif /* LP_POLICY_GLPK_COMPAT_H */
