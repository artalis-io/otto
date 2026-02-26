/*
 * sg_bench_utils.h - Shared utilities for Surge benchmark runners
 *
 * Provides BKS CSV loading, case file collection (with recursive directory
 * scanning), and common helper functions shared across bench_solomon,
 * bench_li_lim, bench_cordeau, and bench_tune.
 */
#ifndef SURGE_SG_BENCH_UTILS_H
#define SURGE_SG_BENCH_UTILS_H

#include "surge.h"

#include <ctype.h>
#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Types ---- */

typedef struct {
    char name[64];
    uint32_t vehicles;
    double distance;
} SGBKSEntry;

typedef struct {
    char *path;   /* heap-allocated, caller frees */
    char *name;   /* points into path (after last '/') */
} SGBenchCase;

/* ---- BKS CSV Loading ---- */

/*
 * Load BKS entries from a CSV file.
 * Format: name,vehicles,distance (lines starting with '#' are comments).
 * Returns number of entries loaded, or -1 on error.
 */
static int sg_load_bks_csv(const char *csv_path, SGBKSEntry *entries, int max_entries) {
    FILE *fp;
    char line[256];
    int count = 0;

    if (!csv_path || !entries || max_entries <= 0) {
        return -1;
    }

    fp = fopen(csv_path, "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && count < max_entries) {
        char *name_start, *comma1, *comma2;
        size_t name_len;

        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        /* Parse: name,vehicles,distance */
        name_start = line;
        comma1 = strchr(name_start, ',');
        if (!comma1) continue;

        name_len = (size_t)(comma1 - name_start);
        if (name_len >= sizeof(entries[0].name)) {
            name_len = sizeof(entries[0].name) - 1;
        }
        memcpy(entries[count].name, name_start, name_len);
        entries[count].name[name_len] = '\0';

        comma2 = strchr(comma1 + 1, ',');
        if (!comma2) continue;

        entries[count].vehicles = (uint32_t)strtoul(comma1 + 1, NULL, 10);
        entries[count].distance = strtod(comma2 + 1, NULL);
        count++;
    }

    fclose(fp);
    return count;
}

/*
 * Find a BKS entry by case key (case-insensitive, alphanumeric only).
 * Returns pointer to matching entry or NULL.
 */
static const SGBKSEntry *sg_bks_find(const SGBKSEntry *entries, int count,
                                      const char *key) {
    int i;
    if (!entries || !key) return NULL;
    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, key) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

/* ---- Case Key Extraction ---- */

/*
 * Extract a normalized case key from a filename.
 * Strips path, extension, and non-alphanumeric chars. Lowercases.
 * For Solomon: "C101.txt" -> "c101"
 * For GH:      "C1_2_1.txt" -> "c121"  (strips underscores)
 * For Li-Lim:  "lc101.txt" -> "lc101"
 * For Cordeau: "a1.txt" -> "a1"
 * Returns 1 on success, 0 on failure.
 */
static int sg_bench_case_key(const char *filename, char *dst, size_t dst_size) {
    const char *base;
    size_t j = 0;

    if (!filename || !dst || dst_size < 2) return 0;

    /* Find basename */
    base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    /* Copy alphanumeric chars, lowercase, stop at '.' */
    while (*base != '\0' && *base != '.' && j + 1 < dst_size) {
        unsigned char c = (unsigned char)*base;
        if (isalnum(c)) {
            dst[j++] = (char)tolower(c);
        }
        base++;
    }
    dst[j] = '\0';
    return j >= 1;
}

/* ---- Case File Collection ---- */

/*
 * Collect .txt files from a directory. If the directory contains
 * subdirectories with .txt files, recurse one level deep.
 *
 * Optional size_filter: if > 0, only include subdirectories whose name
 * matches this number (e.g., size_filter=200 includes "200/" only).
 *
 * Caller must free each cases[i].path and the cases array itself.
 * Returns number of cases found.
 */
static int sg_collect_cases(const char *dir, int size_filter,
                             SGBenchCase **out_cases) {
    glob_t matches;
    char pattern[1024];
    SGBenchCase *cases = NULL;
    int total = 0;
    int capacity = 0;

    if (!dir || !out_cases) return 0;
    *out_cases = NULL;
    memset(&matches, 0, sizeof(matches));

    /* Try flat directory first */
    snprintf(pattern, sizeof(pattern), "%s/*.txt", dir);
    if (glob(pattern, 0, NULL, &matches) == 0 && matches.gl_pathc > 0) {
        /* Check if there are also subdirectories (GH layout) */
        DIR *dp = opendir(dir);
        int has_subdirs = 0;
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp)) != NULL) {
                if (de->d_name[0] == '.') continue;
                if (de->d_type == DT_DIR) {
                    has_subdirs = 1;
                    break;
                }
            }
            closedir(dp);
        }

        if (!has_subdirs) {
            /* Pure flat directory - collect all .txt files */
            capacity = (int)matches.gl_pathc;
            cases = (SGBenchCase *)calloc((size_t)capacity, sizeof(*cases));
            if (!cases) { globfree(&matches); return 0; }

            for (size_t i = 0; i < matches.gl_pathc; i++) {
                cases[total].path = strdup(matches.gl_pathv[i]);
                if (!cases[total].path) continue;
                char *slash = strrchr(cases[total].path, '/');
                cases[total].name = slash ? slash + 1 : cases[total].path;
                total++;
            }
            globfree(&matches);
            *out_cases = cases;
            return total;
        }
        globfree(&matches);
        memset(&matches, 0, sizeof(matches));
    } else {
        globfree(&matches);
        memset(&matches, 0, sizeof(matches));
    }

    /* Recurse into subdirectories */
    {
        DIR *dp = opendir(dir);
        struct dirent *de;
        if (!dp) return 0;

        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (de->d_type != DT_DIR) continue;

            /* Apply size filter if set */
            if (size_filter > 0) {
                int subdir_size = (int)strtol(de->d_name, NULL, 10);
                if (subdir_size != size_filter) continue;
            }

            snprintf(pattern, sizeof(pattern), "%s/%s/*.txt", dir, de->d_name);
            if (glob(pattern, 0, NULL, &matches) != 0 || matches.gl_pathc == 0) {
                globfree(&matches);
                memset(&matches, 0, sizeof(matches));
                continue;
            }

            /* Grow array if needed */
            int needed = total + (int)matches.gl_pathc;
            if (needed > capacity) {
                int new_cap = needed * 2;
                if (new_cap < 64) new_cap = 64;
                SGBenchCase *tmp = (SGBenchCase *)realloc(cases,
                    (size_t)new_cap * sizeof(*cases));
                if (!tmp) { globfree(&matches); break; }
                cases = tmp;
                capacity = new_cap;
            }

            for (size_t i = 0; i < matches.gl_pathc; i++) {
                cases[total].path = strdup(matches.gl_pathv[i]);
                if (!cases[total].path) continue;
                char *slash = strrchr(cases[total].path, '/');
                cases[total].name = slash ? slash + 1 : cases[total].path;
                total++;
            }
            globfree(&matches);
            memset(&matches, 0, sizeof(matches));
        }
        closedir(dp);
    }

    /* Also collect .txt files directly in the top-level dir (mixed layout) */
    snprintf(pattern, sizeof(pattern), "%s/*.txt", dir);
    if (glob(pattern, 0, NULL, &matches) == 0 && matches.gl_pathc > 0) {
        int needed = total + (int)matches.gl_pathc;
        if (needed > capacity) {
            int new_cap = needed * 2;
            SGBenchCase *tmp = (SGBenchCase *)realloc(cases,
                (size_t)new_cap * sizeof(*cases));
            if (tmp) {
                cases = tmp;
                capacity = new_cap;
                for (size_t i = 0; i < matches.gl_pathc; i++) {
                    cases[total].path = strdup(matches.gl_pathv[i]);
                    if (!cases[total].path) continue;
                    char *slash = strrchr(cases[total].path, '/');
                    cases[total].name = slash ? slash + 1 : cases[total].path;
                    total++;
                }
            }
        }
    }
    globfree(&matches);

    *out_cases = cases;
    return total;
}

static int sg_compare_bench_cases(const void *lhs, const void *rhs) {
    const SGBenchCase *a = (const SGBenchCase *)lhs;
    const SGBenchCase *b = (const SGBenchCase *)rhs;
    return strcmp(a->name, b->name);
}

static void sg_free_bench_cases(SGBenchCase *cases, int count) {
    int i;
    if (!cases) return;
    for (i = 0; i < count; i++) {
        free(cases[i].path);
    }
    free(cases);
}

/* ---- Common Helpers ---- */

static double sg_bench_now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char *sg_bench_status_name(SGStatus status) {
    switch (status) {
        case SG_STATUS_OK: return "OK";
        case SG_STATUS_INVALID_ARG: return "INVALID_ARG";
        case SG_STATUS_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case SG_STATUS_INFEASIBLE: return "INFEASIBLE";
        case SG_STATUS_LIMIT: return "LIMIT";
        case SG_STATUS_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
        case SG_STATUS_ERROR:
        default: return "ERROR";
    }
}

__attribute__((unused))
static const char *sg_bench_lexi_vs_bks(uint32_t vehicles, double distance,
                                         const SGBKSEntry *bks) {
    if (!bks) return "N/A";
    if (vehicles < bks->vehicles) return "betterV";
    if (vehicles > bks->vehicles) return "worseV";
    if (distance + 1e-9 < bks->distance) return "betterD";
    if (distance > bks->distance + 1e-9) return "worseD";
    return "match";
}

static int sg_bench_case_selected(const char *case_name, int filter_count,
                                   char **filters) {
    char normalized_case[64];
    int i;
    size_t j;

    if (filter_count <= 0) return 1;

    /* Normalize: strip non-alnum, uppercase */
    j = 0;
    if (case_name) {
        while (*case_name != '\0' && *case_name != '.' && j + 1 < sizeof(normalized_case)) {
            unsigned char c = (unsigned char)*case_name;
            if (isalnum(c)) normalized_case[j++] = (char)toupper(c);
            case_name++;
        }
    }
    normalized_case[j] = '\0';

    for (i = 0; i < filter_count; i++) {
        char nf[64];
        const char *s = filters[i];
        size_t k = 0;
        while (*s != '\0' && *s != '.' && k + 1 < sizeof(nf)) {
            unsigned char c = (unsigned char)*s;
            if (isalnum(c)) nf[k++] = (char)toupper(c);
            s++;
        }
        nf[k] = '\0';
        if (strcmp(normalized_case, nf) == 0) return 1;
    }
    return 0;
}

#endif /* SURGE_SG_BENCH_UTILS_H */
