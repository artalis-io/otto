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
    LP_GLPK_SMCP_METHOD_DUAL = 2
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
    LP_GLPK_SMCP_BASIS_STD = 1
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
                                         int *bfcp_update_limit_io,
                                         double *bfcp_pivot_tol_io,
                                         double *bfcp_growth_guard_io,
                                         int *soft_lu_cost_gate_enabled_io,
                                         int *periodic_cost_gate_enabled_io);

#endif /* LP_POLICY_GLPK_COMPAT_H */
