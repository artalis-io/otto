#include "nx_compute.h"
#include "sh_eov.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ========================================================================
 * Built-in Compute Functions
 * ======================================================================== */

/**
 * eov_to_wgs84 - Convert EOV (Hungarian) coordinates to WGS84
 *
 * Sources: [EOV Y, EOV X]
 * Outputs: [lat, lon]
 */
static int nx_compute_eov_to_wgs84(const char **sources, int nsources,
                                    char outputs[][256], int max_outputs)
{
    if (nsources < 2 || max_outputs < 2) return -1;
    if (!sources[0] || !sources[1]) return -1;

    double eov_y = atof(sources[0]);
    double eov_x = atof(sources[1]);
    double lat, lon;

    if (sh_eov_to_wgs84(eov_y, eov_x, &lat, &lon) != 0) {
        return -1;
    }

    snprintf(outputs[0], 256, "%.6f", lat);
    snprintf(outputs[1], 256, "%.6f", lon);
    return 2;
}

/**
 * dms_to_dd - Convert degrees/minutes/seconds to decimal degrees
 *
 * Sources: [degrees, minutes, seconds]
 * Outputs: [decimal degrees]
 */
static int nx_compute_dms_to_dd(const char **sources, int nsources,
                                 char outputs[][256], int max_outputs)
{
    if (nsources < 3 || max_outputs < 1) return -1;
    if (!sources[0] || !sources[1] || !sources[2]) return -1;

    double degrees = atof(sources[0]);
    double minutes = atof(sources[1]);
    double seconds = atof(sources[2]);

    double dd = degrees + minutes / 60.0 + seconds / 3600.0;
    snprintf(outputs[0], 256, "%.6f", dd);
    return 1;
}

/**
 * coalesce - Return first non-empty value
 *
 * Sources: [value1, value2, ..., valueN]
 * Outputs: [first non-empty value]
 */
static int nx_compute_coalesce(const char **sources, int nsources,
                                 char outputs[][256], int max_outputs)
{
    if (nsources < 1 || max_outputs < 1) return -1;

    for (int i = 0; i < nsources; i++) {
        if (sources[i] && sources[i][0] != '\0') {
            snprintf(outputs[0], 256, "%s", sources[i]);
            return 1;
        }
    }

    /* All sources empty, return empty string */
    outputs[0][0] = '\0';
    return 1;
}

/**
 * phone_normalize - Normalize Hungarian phone numbers to E.164 format
 *
 * Sources: [raw phone number]
 * Outputs: [+36... format]
 *
 * Strips non-digit characters, then:
 * - "06..." -> "+36..."
 * - "36..." -> "+36..."
 * - "+36..." -> "+36..." (unchanged)
 */
static int nx_compute_phone_normalize(const char **sources, int nsources,
                                        char outputs[][256], int max_outputs)
{
    if (nsources < 1 || max_outputs < 1) return -1;
    if (!sources[0]) return -1;

    /* Strip all non-digit characters except leading + */
    char digits[256];
    int pos = 0;
    int first_char = 1;

    for (const char *p = sources[0]; *p && pos < 255; p++) {
        if (isdigit((unsigned char)*p)) {
            digits[pos++] = *p;
            first_char = 0;
        } else if (first_char && *p == '+') {
            digits[pos++] = *p;
            first_char = 0;
        }
    }
    digits[pos] = '\0';

    /* Normalize to E.164 */
    if (digits[0] == '0' && digits[1] == '6') {
        /* "06..." -> "+36..." */
        snprintf(outputs[0], 256, "+36%s", digits + 2);
    } else if (digits[0] == '3' && digits[1] == '6') {
        /* "36..." -> "+36..." */
        snprintf(outputs[0], 256, "+%s", digits);
    } else if (digits[0] == '+' && digits[1] == '3' && digits[2] == '6') {
        /* "+36..." -> unchanged */
        snprintf(outputs[0], 256, "%s", digits);
    } else {
        /* Invalid format, return as-is */
        snprintf(outputs[0], 256, "%s", sources[0]);
    }

    return 1;
}

/* ========================================================================
 * Registry
 * ======================================================================== */

typedef struct {
    const char *name;
    NxComputeFunc func;
} NxComputeEntry;

static const NxComputeEntry nx_compute_registry[] = {
    {"eov_to_wgs84", nx_compute_eov_to_wgs84},
    {"dms_to_dd", nx_compute_dms_to_dd},
    {"coalesce", nx_compute_coalesce},
    {"phone_normalize", nx_compute_phone_normalize},
    {NULL, NULL}  /* Sentinel */
};

NxComputeFunc nx_compute_find(const char *name)
{
    if (!name) return NULL;

    for (int i = 0; nx_compute_registry[i].name; i++) {
        if (strcmp(nx_compute_registry[i].name, name) == 0) {
            return nx_compute_registry[i].func;
        }
    }

    return NULL;
}
