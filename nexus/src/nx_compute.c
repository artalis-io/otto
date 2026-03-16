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

    char *end_y, *end_x;
    double eov_y = strtod(sources[0], &end_y);
    double eov_x = strtod(sources[1], &end_x);
    if (end_y == sources[0] || *end_y != '\0') return -1;
    if (end_x == sources[1] || *end_x != '\0') return -1;
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

    char *end_d, *end_m, *end_s;
    double degrees = strtod(sources[0], &end_d);
    double minutes = strtod(sources[1], &end_m);
    double seconds = strtod(sources[2], &end_s);
    if (end_d == sources[0] || *end_d != '\0') return -1;
    if (end_m == sources[1] || *end_m != '\0') return -1;
    if (end_s == sources[2] || *end_s != '\0') return -1;

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

/**
 * zip_to_region - Map Hungarian ZIP code to postal region name
 *
 * Sources: [ZIP code string]
 * Outputs: [region name]
 *
 * Hungarian ZIP first digit → region:
 *   1 = Budapest, 2 = Pest, 3 = Northern Hungary,
 *   4 = Northern Great Plain, 5 = Southern Great Plain,
 *   6 = Bacs-Kiskun, 7 = Southern Transdanubia,
 *   8 = Central/Western Transdanubia, 9 = Western Transdanubia
 */
static int nx_compute_zip_to_region(const char **sources, int nsources,
                                     char outputs[][256], int max_outputs)
{
    if (nsources < 1 || max_outputs < 1) return -1;
    if (!sources[0] || !sources[0][0]) return -1;

    /* Validate: must be 4 digits, first digit 1-9 */
    const char *zip = sources[0];
    int len = 0;
    for (const char *p = zip; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            snprintf(outputs[0], 256, "%s", zip);
            return 1; /* Return as-is if not numeric */
        }
        len++;
    }
    if (len != 4) {
        snprintf(outputs[0], 256, "%s", zip);
        return 1;
    }

    static const char *regions[] = {
        NULL,                           /* 0 - invalid */
        "Budapest",                     /* 1xxx */
        "Pest",                         /* 2xxx */
        "Northern Hungary",             /* 3xxx */
        "Northern Great Plain",         /* 4xxx */
        "Southern Great Plain",         /* 5xxx */
        "Bacs-Kiskun",                  /* 6xxx */
        "Southern Transdanubia",        /* 7xxx */
        "Central/Western Transdanubia", /* 8xxx */
        "Western Transdanubia"          /* 9xxx */
    };

    int first = zip[0] - '0';
    if (first < 1 || first > 9) {
        snprintf(outputs[0], 256, "%s", zip);
        return 1;
    }

    snprintf(outputs[0], 256, "%s", regions[first]);
    return 1;
}

/**
 * opening_hours_normalize - Normalize Hungarian opening hours to ISO format
 *
 * Sources: [Hungarian opening hours string]
 * Outputs: [ISO-style opening hours]
 *
 * Input formats:
 *   "H-P: 8-17"      → "Mo-Fr 08:00-17:00"
 *   "H-Szo: 8:00-20" → "Mo-Sa 08:00-20:00"
 *   "H-V: 0-24"      → "Mo-Su 00:00-24:00"
 *
 * Hungarian day abbreviations:
 *   H=Monday, K=Tuesday, Sze=Wednesday, Cs=Thursday,
 *   P=Friday, Szo=Saturday, V=Sunday
 */
static int nx_compute_opening_hours(const char **sources, int nsources,
                                     char outputs[][256], int max_outputs)
{
    if (nsources < 1 || max_outputs < 1) return -1;
    if (!sources[0] || !sources[0][0]) return -1;

    const char *src = sources[0];

    /* Hungarian day abbreviations → English ISO abbreviations */
    typedef struct { const char *hu; int hu_len; const char *en; } DayMap;
    static const DayMap days[] = {
        {"Sze", 3, "We"}, {"Szo", 3, "Sa"}, /* 3-char first (greedy) */
        {"Cs",  2, "Th"},
        {"H",   1, "Mo"}, {"K",   1, "Tu"},
        {"P",   1, "Fr"}, {"V",   1, "Su"},
        {NULL,  0, NULL}
    };

    /* Try to find day-day range: look for "X-Y:" or "X-Y " */
    const char *p = src;
    while (*p == ' ') p++;

    /* Parse start day */
    const char *start_en = NULL;
    for (int i = 0; days[i].hu; i++) {
        if (strncmp(p, days[i].hu, (size_t)days[i].hu_len) == 0) {
            start_en = days[i].en;
            p += days[i].hu_len;
            break;
        }
    }

    if (!start_en) {
        /* Can't parse, return as-is */
        snprintf(outputs[0], 256, "%s", src);
        return 1;
    }

    /* Expect '-' separator */
    if (*p != '-') {
        snprintf(outputs[0], 256, "%s", src);
        return 1;
    }
    p++;

    /* Parse end day */
    const char *end_en = NULL;
    for (int i = 0; days[i].hu; i++) {
        if (strncmp(p, days[i].hu, (size_t)days[i].hu_len) == 0) {
            end_en = days[i].en;
            p += days[i].hu_len;
            break;
        }
    }

    if (!end_en) {
        snprintf(outputs[0], 256, "%s", src);
        return 1;
    }

    /* Skip ':' or ' ' separator to time part */
    while (*p == ':' || *p == ' ') p++;

    /* Parse start time (H, HH, H:MM, HH:MM) */
    int start_h = 0, start_m = 0;
    if (!isdigit((unsigned char)*p)) {
        snprintf(outputs[0], 256, "%s", src);
        return 1;
    }
    start_h = *p++ - '0';
    if (isdigit((unsigned char)*p)) start_h = start_h * 10 + (*p++ - '0');
    if (*p == ':') {
        p++;
        if (isdigit((unsigned char)*p)) {
            start_m = *p++ - '0';
            if (isdigit((unsigned char)*p)) start_m = start_m * 10 + (*p++ - '0');
        }
    }

    /* Expect '-' */
    if (*p != '-') {
        snprintf(outputs[0], 256, "%s", src);
        return 1;
    }
    p++;

    /* Parse end time */
    int end_h = 0, end_m = 0;
    if (!isdigit((unsigned char)*p)) {
        snprintf(outputs[0], 256, "%s", src);
        return 1;
    }
    end_h = *p++ - '0';
    if (isdigit((unsigned char)*p)) end_h = end_h * 10 + (*p++ - '0');
    if (*p == ':') {
        p++;
        if (isdigit((unsigned char)*p)) {
            end_m = *p++ - '0';
            if (isdigit((unsigned char)*p)) end_m = end_m * 10 + (*p++ - '0');
        }
    }

    snprintf(outputs[0], 256, "%s-%s %02d:%02d-%02d:%02d",
             start_en, end_en, start_h, start_m, end_h, end_m);
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
    {"zip_to_region", nx_compute_zip_to_region},
    {"opening_hours", nx_compute_opening_hours},
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
