/*
 * FuelWise Benchmark - GLPK Comparison Helper
 *
 * Exports MILP as LP file and solves with glpsol for comparison.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "fw_bench.h"
#include "sh_pal.h"
#include "fw_refuel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
  #include <io.h>
  /* Windows has no execute bit, so _access has no X_OK: existence is the
   * closest honest answer, and the paths probed here are POSIX ones anyway. */
  #ifndef X_OK
    #define X_OK 0
  #endif
#else
  #include <unistd.h>
#endif
#include <math.h>
#include <time.h>

/* ============================================================================
 * GLPK Result Type
 * ============================================================================ */

typedef struct {
    int solved;
    double objective;
    double solve_time_ms;
} FWGlpkResult;

/* ============================================================================
 * GLPK Solver Helper
 * ============================================================================ */

static double get_time_ms(void)
{
    return (double)sh_monotonic_ns() / 1.0e6;
}

static const char *fw_glpsol_path(void)
{
    const char *env_path = getenv("FW_GLPSOL_PATH");
    if (env_path && env_path[0] != '\0') {
        return env_path;
    }

    if (access("/opt/homebrew/bin/glpsol", X_OK) == 0) {
        return "/opt/homebrew/bin/glpsol";
    }

    return "glpsol";
}

/*
 * Solve a FuelWise MILP problem using GLPK (glpsol).
 *
 * 1. Exports the raw MILP (no hints) as an LP file via fw_export_milp_lp()
 * 2. Shells out to glpsol to solve
 * 3. Parses timing and objective from GLPK output
 *
 * Returns: 0 on success, -1 on error
 */
/* c-audit: popen intentional — benchmark script, not library */
int fw_glpk_solve(const FWRefuelProblem *problem, FWGlpkResult *result)
{
    if (!problem || !result) return -1;

    memset(result, 0, sizeof(FWGlpkResult));

    /* Create temp files for LP and solution. The directory comes from the
     * PAL rather than a literal "/tmp": under MSYS2 this is a native Windows
     * binary, so "/tmp/x" would mean "C:\\tmp\\x". */
    char base_path[320];
    if (sh_pal_make_tempfile("fw_bench_", base_path, sizeof(base_path)) != 0) return -1;
    unlink(base_path);  /* We just need the unique name */

    char lp_path[328];
    char sol_path[328];
    snprintf(lp_path, sizeof(lp_path), "%s.lp", base_path);
    snprintf(sol_path, sizeof(sol_path), "%s.sol", base_path);

    /* Export MILP model as LP file (without domain hints) */
    if (fw_export_milp_lp(problem, lp_path) != 0) {
        return -1;
    }

    /* Build glpsol command */
    char cmd[512];
    /*
     * Quoting here is platform-specific, and single quotes are wrong on both.
     *
     * popen() runs the command through cmd.exe on Windows, which does not
     * treat '...' as quoting at all -- it goes looking for a program named
     * 'glpsol', quotes included. Double quotes are quoting in cmd and in sh
     * alike, but cmd has one more rule: when the command line begins with a
     * quote it strips the first and the last one from the whole line, which
     * mangles a line that quotes three arguments. The fix is the documented
     * one, an extra outer pair with the redirection inside it.
     */
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
             "\"\"%s\" --lp \"%s\" -o \"%s\" 2>&1\"",
             fw_glpsol_path(), lp_path, sol_path);
#else
    snprintf(cmd, sizeof(cmd),
             "\"%s\" --lp \"%s\" -o \"%s\" 2>&1",
             fw_glpsol_path(), lp_path, sol_path);
#endif

    /* Time the GLPK solve */
    double t0 = get_time_ms();

    /* c-audit: popen intentional — benchmark script, not library */
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        unlink(lp_path);
        return -1;
    }

    /* Read glpsol output — look for timing info */
    char line[256];
    double glpk_reported_time = 0;
    while (fgets(line, sizeof(line), pipe)) {
        if (strstr(line, "Time used:")) {
            sscanf(line, "Time used: %lf", &glpk_reported_time);
        }
    }

    int ret = pclose(pipe);
    double wall_time = get_time_ms() - t0;

    /* Use wall time as primary measurement (includes I/O overhead, but
     * more reliable than GLPK's self-reported time which has low resolution) */
    result->solve_time_ms = wall_time;

    if (ret != 0) {
        unlink(lp_path);
        unlink(sol_path);
        return -1;
    }

    /* Parse GLPK solution file */
    FILE *f = fopen(sol_path, "r");
    if (!f) {
        unlink(lp_path);
        unlink(sol_path);
        return -1;
    }

    int found_optimal = 0;
    double objective = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Look for status line: "Status:     INTEGER OPTIMAL" or "Status:     OPTIMAL" */
        if (strstr(line, "Status:")) {
            if (strstr(line, "OPTIMAL") || strstr(line, "INTEGER OPTIMAL")) {
                found_optimal = 1;
            }
        }
        /* Look for objective: "Objective:  obj = 123.456 (MINimum)" */
        if (strstr(line, "Objective:")) {
            char *eq = strstr(line, "=");
            if (eq) {
                objective = atof(eq + 1);
            }
        }
    }

    fclose(f);

    /* Clean up temp files */
    unlink(lp_path);
    unlink(sol_path);

    if (found_optimal) {
        result->solved = 1;
        result->objective = objective;
        return 0;
    }

    return -1;
}
