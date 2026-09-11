/*
 * Surge API Handler Implementation
 *
 * Transport-agnostic request handling for VRP/PDPTW solving.
 */

#include "sg_api.h"
#include "surge.h"
#include "sh_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * API Context (stateless — reserved for future use)
 * ============================================================================ */

struct SGAPIContext {
    int _unused;
};

SGAPIContext *sg_api_create(void) {
    return calloc(1, sizeof(SGAPIContext));
}

void sg_api_free(SGAPIContext *ctx) {
    free(ctx);
}

/* ============================================================================
 * JSON Error Response Helper
 * ============================================================================ */

static char *make_error_json(int status_code, const char *message, size_t *out_len) {
    ShJsonBuf jb;
    ShJsonWriter w;

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "status", "ERROR");
    sh_json_write_kv_int(&w, "code", status_code);
    sh_json_write_kv_string(&w, "error", message);
    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NULL;
    }

    if (out_len) {
        *out_len = jb.len;
    }
    return sh_json_buf_take(&jb);
}

/* ============================================================================
 * JSON Model Builders
 * ============================================================================ */

static SGStatus build_config(SGContext *ctx, const ShJsonValue *cfg_val) {
    SGConfig cfg;

    sg_config_default(&cfg);

    if (cfg_val && sh_json_type(cfg_val) == SH_JSON_OBJECT) {
        ShJsonValue *v;

        v = sh_json_get(cfg_val, "max_iterations");
        if (v) cfg.max_iterations = sh_json_as_int(v, cfg.max_iterations);

        v = sh_json_get(cfg_val, "max_time_seconds");
        if (v) cfg.max_time_seconds = sh_json_as_int(v, cfg.max_time_seconds);

        v = sh_json_get(cfg_val, "segment_size");
        if (v) cfg.segment_size = sh_json_as_int(v, cfg.segment_size);

        v = sh_json_get(cfg_val, "q_min");
        if (v) cfg.q_min = sh_json_as_int(v, cfg.q_min);

        v = sh_json_get(cfg_val, "q_max");
        if (v) cfg.q_max = sh_json_as_int(v, cfg.q_max);

        v = sh_json_get(cfg_val, "seed");
        if (v) cfg.seed = (uint64_t)sh_json_as_double(v, (double)cfg.seed);

        v = sh_json_get(cfg_val, "deterministic");
        if (v) cfg.deterministic = sh_json_as_bool(v, cfg.deterministic);

        v = sh_json_get(cfg_val, "accept_type");
        if (v) {
            const char *at = sh_json_as_string(v, "sa");
            if (strcmp(at, "rrt") == 0) cfg.accept_type = SG_ACCEPT_RRT;
            else if (strcmp(at, "improving") == 0) cfg.accept_type = SG_ACCEPT_IMPROVING;
            else cfg.accept_type = SG_ACCEPT_SA;
        }

        v = sh_json_get(cfg_val, "lexicographic_objective");
        if (v) cfg.lexicographic_objective = sh_json_as_bool(v, cfg.lexicographic_objective);

        v = sh_json_get(cfg_val, "adaptive_q");
        if (v) cfg.adaptive_q = sh_json_as_bool(v, cfg.adaptive_q);

        v = sh_json_get(cfg_val, "use_insertion_cache");
        if (v) cfg.use_insertion_cache = sh_json_as_bool(v, cfg.use_insertion_cache);
    }

    return sg_set_config(ctx, &cfg);
}

static int build_locations(SGContext *ctx, const ShJsonValue *locs_arr) {
    size_t i, count;

    if (!locs_arr || sh_json_type(locs_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(locs_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *loc = sh_json_array_get(locs_arr, i);
        uint32_t id;
        double x, y;

        if (!loc || sh_json_type(loc) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_location(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        x = sh_json_as_double(sh_json_get(loc, "x"), 0.0);
        y = sh_json_as_double(sh_json_get(loc, "y"), 0.0);
        if (sg_location_set_coords(ctx, id, x, y) != SG_STATUS_OK) {
            return -1;
        }
    }

    return 0;
}

static int build_speed_profiles(SGContext *ctx, const ShJsonValue *arr) {
    size_t i, count;

    if (!arr || sh_json_type(arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *prof = sh_json_array_get(arr, i);
        ShJsonValue *entries;
        uint32_t pid;
        size_t j, entry_count;

        if (!prof || sh_json_type(prof) != SH_JSON_OBJECT) {
            return -1;
        }

        pid = sg_add_speed_profile(ctx);
        if (pid == UINT32_MAX) return -1;

        entries = sh_json_get(prof, "entries");
        if (!entries || sh_json_type(entries) != SH_JSON_ARRAY) {
            return -1;
        }

        entry_count = sh_json_array_len(entries);
        for (j = 0; j < entry_count; j++) {
            ShJsonValue *entry = sh_json_array_get(entries, j);
            double start_time, multiplier;
            if (!entry || sh_json_type(entry) != SH_JSON_OBJECT) {
                return -1;
            }
            start_time = sh_json_as_double(sh_json_get(entry, "start_time"), 0.0);
            multiplier = sh_json_as_double(sh_json_get(entry, "multiplier"), 1.0);
            if (sg_speed_profile_add_entry(ctx, pid, start_time, multiplier) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_travel_profiles(SGContext *ctx, const ShJsonValue *arr) {
    size_t i, count;

    if (!arr || sh_json_type(arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *prof = sh_json_array_get(arr, i);
        ShJsonValue *dist_arr, *dur_arr, *sp_v;
        uint32_t pid;
        double *distances = NULL;
        double *durations = NULL;
        size_t n, j;
        int location_count;

        if (!prof || sh_json_type(prof) != SH_JSON_OBJECT) {
            return -1;
        }

        pid = sg_add_travel_profile(ctx);
        if (pid == UINT32_MAX) return -1;

        dist_arr = sh_json_get(prof, "distance_matrix");
        dur_arr = sh_json_get(prof, "duration_matrix");
        if (dist_arr || dur_arr) {
            /* Determine location_count from whichever matrix is present */
            ShJsonValue *mat = dist_arr ? dist_arr : dur_arr;
            if (sh_json_type(mat) != SH_JSON_ARRAY) return -1;
            n = sh_json_array_len(mat);
            location_count = (int)sqrt((double)n);
            if ((size_t)location_count * (size_t)location_count != n || location_count <= 0) {
                return -1;
            }
            /* Guard the allocation size against wraparound on 32-bit (WASM)
             * targets before allocating n doubles from the untrusted length. */
            if (n > SIZE_MAX / sizeof(double)) return -1;

            if (dist_arr) {
                if (sh_json_type(dist_arr) != SH_JSON_ARRAY || sh_json_array_len(dist_arr) != n) {
                    return -1;
                }
                distances = (double *)malloc(n * sizeof(double));
                if (!distances) return -1;
                for (j = 0; j < n; j++) {
                    distances[j] = sh_json_as_double(sh_json_array_get(dist_arr, j), 0.0);
                }
            }

            if (dur_arr) {
                if (sh_json_type(dur_arr) != SH_JSON_ARRAY || sh_json_array_len(dur_arr) != n) {
                    free(distances);
                    return -1;
                }
                durations = (double *)malloc(n * sizeof(double));
                if (!durations) {
                    free(distances);
                    return -1;
                }
                for (j = 0; j < n; j++) {
                    durations[j] = sh_json_as_double(sh_json_array_get(dur_arr, j), 0.0);
                }
            }

            if (sg_travel_profile_set_matrices(ctx, pid, (uint32_t)location_count,
                                                distances, durations) != SG_STATUS_OK) {
                free(distances);
                free(durations);
                return -1;
            }
            free(distances);
            free(durations);
        }

        sp_v = sh_json_get(prof, "speed_profile_id");
        if (sp_v) {
            uint32_t sp_id = (uint32_t)sh_json_as_int(sp_v, 0);
            if (sg_travel_profile_set_speed_profile(ctx, pid, sp_id) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Per-profile time brackets */
        {
            ShJsonValue *tb_arr = sh_json_get(prof, "time_brackets");
            if (tb_arr && sh_json_type(tb_arr) == SH_JSON_ARRAY) {
                size_t tb_count = sh_json_array_len(tb_arr);
                size_t tb_i;
                for (tb_i = 0; tb_i < tb_count; tb_i++) {
                    ShJsonValue *tb = sh_json_array_get(tb_arr, tb_i);
                    ShJsonValue *st_v2, *dur_v2, *dist_v2;
                    double st2;
                    double *tb_dist = NULL;
                    double *tb_dur = NULL;
                    size_t tb_n, tb_j2;
                    int tb_loc_count;

                    if (!tb || sh_json_type(tb) != SH_JSON_OBJECT) return -1;

                    st_v2 = sh_json_get(tb, "start_time");
                    if (!st_v2) return -1;
                    st2 = sh_json_as_double(st_v2, 0.0);

                    dur_v2 = sh_json_get(tb, "durations");
                    if (!dur_v2 || sh_json_type(dur_v2) != SH_JSON_ARRAY) return -1;
                    tb_n = sh_json_array_len(dur_v2);
                    tb_loc_count = (int)sqrt((double)tb_n);
                    if ((size_t)tb_loc_count * (size_t)tb_loc_count != tb_n || tb_loc_count <= 0) {
                        return -1;
                    }
                    if (tb_n > SIZE_MAX / sizeof(double)) return -1;  /* alloc guard (WASM32) */

                    tb_dur = (double *)malloc(tb_n * sizeof(double));
                    if (!tb_dur) return -1;
                    for (tb_j2 = 0; tb_j2 < tb_n; tb_j2++) {
                        tb_dur[tb_j2] = sh_json_as_double(sh_json_array_get(dur_v2, tb_j2), 0.0);
                    }

                    dist_v2 = sh_json_get(tb, "distances");
                    if (dist_v2 && sh_json_type(dist_v2) == SH_JSON_ARRAY) {
                        if (sh_json_array_len(dist_v2) != tb_n) {
                            free(tb_dur);
                            return -1;
                        }
                        tb_dist = (double *)malloc(tb_n * sizeof(double));
                        if (!tb_dist) {
                            free(tb_dur);
                            return -1;
                        }
                        for (tb_j2 = 0; tb_j2 < tb_n; tb_j2++) {
                            tb_dist[tb_j2] = sh_json_as_double(sh_json_array_get(dist_v2, tb_j2), 0.0);
                        }
                    }

                    if (sg_travel_profile_add_time_bracket(ctx, pid, st2,
                            (uint32_t)tb_loc_count, tb_dist, tb_dur) != SG_STATUS_OK) {
                        free(tb_dist);
                        free(tb_dur);
                        return -1;
                    }
                    free(tb_dist);
                    free(tb_dur);
                }
            }
        }
    }

    return 0;
}

static int build_travel(SGContext *ctx, const ShJsonValue *travel_val) {
    ShJsonValue *v;
    int location_count;
    size_t n;
    double *distances = NULL;
    double *durations = NULL;
    size_t i;
    SGStatus status;

    if (!travel_val || sh_json_type(travel_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(travel_val, "location_count");
    if (!v) return -1;
    location_count = sh_json_as_int(v, 0);
    if (location_count <= 0) return -1;
    n = (size_t)location_count * (size_t)location_count;

    v = sh_json_get(travel_val, "distances");
    if (!v || sh_json_type(v) != SH_JSON_ARRAY || sh_json_array_len(v) != n) {
        return -1;
    }
    distances = (double *)malloc(n * sizeof(double));
    if (!distances) return -1;
    for (i = 0; i < n; i++) {
        distances[i] = sh_json_as_double(sh_json_array_get(v, i), 0.0);
    }

    v = sh_json_get(travel_val, "durations");
    if (!v || sh_json_type(v) != SH_JSON_ARRAY || sh_json_array_len(v) != n) {
        free(distances);
        return -1;
    }
    durations = (double *)malloc(n * sizeof(double));
    if (!durations) {
        free(distances);
        return -1;
    }
    for (i = 0; i < n; i++) {
        durations[i] = sh_json_as_double(sh_json_array_get(v, i), 0.0);
    }

    status = sg_set_travel_matrix(ctx, (uint32_t)location_count, distances, durations);
    free(distances);
    free(durations);
    if (status != SG_STATUS_OK) return -1;

    /* Optional global speed profile */
    v = sh_json_get(travel_val, "speed_profile_id");
    if (v) {
        uint32_t sp_id = (uint32_t)sh_json_as_int(v, 0);
        if (sg_set_global_speed_profile(ctx, sp_id) != SG_STATUS_OK) {
            return -1;
        }
    }

    /* Optional global time brackets */
    v = sh_json_get(travel_val, "time_brackets");
    if (v && sh_json_type(v) == SH_JSON_ARRAY) {
        size_t tb_count = sh_json_array_len(v);
        size_t tb_i;
        for (tb_i = 0; tb_i < tb_count; tb_i++) {
            ShJsonValue *tb = sh_json_array_get(v, tb_i);
            ShJsonValue *st_v, *dur_v, *dist_v;
            double st;
            double *tb_distances = NULL;
            double *tb_durations = NULL;
            size_t tb_j;

            if (!tb || sh_json_type(tb) != SH_JSON_OBJECT) return -1;

            st_v = sh_json_get(tb, "start_time");
            if (!st_v) return -1;
            st = sh_json_as_double(st_v, 0.0);

            dur_v = sh_json_get(tb, "durations");
            if (!dur_v || sh_json_type(dur_v) != SH_JSON_ARRAY ||
                sh_json_array_len(dur_v) != n) {
                return -1;
            }
            tb_durations = (double *)malloc(n * sizeof(double));
            if (!tb_durations) return -1;
            for (tb_j = 0; tb_j < n; tb_j++) {
                tb_durations[tb_j] = sh_json_as_double(sh_json_array_get(dur_v, tb_j), 0.0);
            }

            dist_v = sh_json_get(tb, "distances");
            if (dist_v && sh_json_type(dist_v) == SH_JSON_ARRAY) {
                if (sh_json_array_len(dist_v) != n) {
                    free(tb_durations);
                    return -1;
                }
                tb_distances = (double *)malloc(n * sizeof(double));
                if (!tb_distances) {
                    free(tb_durations);
                    return -1;
                }
                for (tb_j = 0; tb_j < n; tb_j++) {
                    tb_distances[tb_j] = sh_json_as_double(sh_json_array_get(dist_v, tb_j), 0.0);
                }
            }

            if (sg_set_travel_time_bracket(ctx, st, (uint32_t)location_count,
                                            tb_distances, tb_durations) != SG_STATUS_OK) {
                free(tb_distances);
                free(tb_durations);
                return -1;
            }
            free(tb_distances);
            free(tb_durations);
        }
    }

    return 0;
}

static int build_zones(SGContext *ctx, const ShJsonValue *zones_val) {
    ShJsonValue *v;
    int zone_count;
    size_t n, i;
    double *matrix;
    SGStatus status;

    if (!zones_val || sh_json_type(zones_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(zones_val, "count");
    if (!v) return -1;
    zone_count = sh_json_as_int(v, 0);
    if (zone_count <= 0) return -1;
    n = (size_t)zone_count * (size_t)zone_count;

    v = sh_json_get(zones_val, "distances");
    if (!v || sh_json_type(v) != SH_JSON_ARRAY || sh_json_array_len(v) != n) {
        return -1;
    }
    if (n > SIZE_MAX / sizeof(double)) return -1;  /* alloc guard (WASM32) */

    matrix = (double *)malloc(n * sizeof(double));
    if (!matrix) return -1;
    for (i = 0; i < n; i++) {
        matrix[i] = sh_json_as_double(sh_json_array_get(v, i), 0.0);
    }

    status = sg_set_zone_distance_matrix(ctx, (uint32_t)zone_count, matrix);
    free(matrix);
    return (status == SG_STATUS_OK) ? 0 : -1;
}

static int build_compartment_types(SGContext *ctx, const ShJsonValue *ct_arr) {
    size_t i, count;

    if (!ct_arr || sh_json_type(ct_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(ct_arr);
    for (i = 0; i < count; i++) {
        uint32_t tid;
        if (sg_add_compartment_type(ctx, &tid) != SG_STATUS_OK) {
            return -1;
        }
    }

    return 0;
}

static int build_commodities(SGContext *ctx, const ShJsonValue *comm_val) {
    ShJsonValue *v;
    int count, i;
    ShJsonValue *conflicts;
    size_t ci, clen;

    if (!comm_val || sh_json_type(comm_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(comm_val, "count");
    if (!v) return -1;
    count = sh_json_as_int(v, 0);
    if (count <= 0) return -1;

    for (i = 0; i < count; i++) {
        uint32_t cid;
        if (sg_add_commodity(ctx, &cid) != SG_STATUS_OK) {
            return -1;
        }
    }

    conflicts = sh_json_get(comm_val, "conflicts");
    if (conflicts && sh_json_type(conflicts) == SH_JSON_ARRAY) {
        clen = sh_json_array_len(conflicts);
        for (ci = 0; ci < clen; ci++) {
            ShJsonValue *pair = sh_json_array_get(conflicts, ci);
            uint32_t a, b;
            if (!pair || sh_json_type(pair) != SH_JSON_ARRAY || sh_json_array_len(pair) != 2) {
                return -1;
            }
            a = (uint32_t)sh_json_as_int(sh_json_array_get(pair, 0), 0);
            b = (uint32_t)sh_json_as_int(sh_json_array_get(pair, 1), 0);
            if (sg_commodity_set_conflict(ctx, a, b) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_exclusion_groups(SGContext *ctx, const ShJsonValue *eg_val) {
    ShJsonValue *v;
    int count, i;

    if (!eg_val || sh_json_type(eg_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(eg_val, "count");
    if (!v) return -1;
    count = sh_json_as_int(v, 0);
    if (count <= 0) return -1;

    for (i = 0; i < count; i++) {
        uint32_t gid;
        if (sg_add_exclusion_group(ctx, &gid) != SG_STATUS_OK) {
            return -1;
        }
    }

    return 0;
}

static int build_setup_times(SGContext *ctx, const ShJsonValue *st_val) {
    ShJsonValue *v;
    int num_classes;
    size_t n, i, j;
    ShJsonValue *matrix_arr;

    if (!st_val || sh_json_type(st_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(st_val, "num_classes");
    if (!v) return -1;
    num_classes = sh_json_as_int(v, 0);
    if (num_classes <= 0) return -1;

    if (sg_set_num_setup_classes(ctx, (uint32_t)num_classes) != SG_STATUS_OK) {
        return -1;
    }

    matrix_arr = sh_json_get(st_val, "matrix");
    if (matrix_arr && sh_json_type(matrix_arr) == SH_JSON_ARRAY) {
        n = (size_t)num_classes * (size_t)num_classes;
        if (sh_json_array_len(matrix_arr) != n) {
            return -1;
        }
        for (i = 0; i < (size_t)num_classes; i++) {
            for (j = 0; j < (size_t)num_classes; j++) {
                double secs = sh_json_as_double(
                    sh_json_array_get(matrix_arr, i * (size_t)num_classes + j), 0.0);
                if (sg_set_setup_time(ctx, (uint32_t)i + 1, (uint32_t)j + 1, secs) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int build_depots(SGContext *ctx, const ShJsonValue *depots_arr) {
    size_t i, count;

    if (!depots_arr || sh_json_type(depots_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(depots_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *d = sh_json_array_get(depots_arr, i);
        uint32_t id;
        ShJsonValue *v;

        if (!d || sh_json_type(d) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_depot(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(d, "x");
        if (v) {
            double x = sh_json_as_double(v, 0.0);
            double y = sh_json_as_double(sh_json_get(d, "y"), 0.0);
            if (sg_depot_set_location(ctx, id, x, y) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(d, "location_id");
        if (v) {
            uint32_t loc_id = (uint32_t)sh_json_as_int(v, 0);
            if (sg_depot_set_location_id(ctx, id, loc_id) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(d, "tw_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(d, "tw_late"), 0);
            if (sg_depot_set_time_window(ctx, id, early, late) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(d, "max_simultaneous");
        if (v) {
            uint32_t ms = (uint32_t)sh_json_as_int(v, 0);
            if (sg_depot_set_max_simultaneous(ctx, id, ms) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_vehicles(SGContext *ctx, const ShJsonValue *vehicles_arr) {
    size_t i, count;

    if (!vehicles_arr || sh_json_type(vehicles_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(vehicles_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *veh = sh_json_array_get(vehicles_arr, i);
        uint32_t id;
        ShJsonValue *v;

        if (!veh || sh_json_type(veh) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_vehicle(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(veh, "start_depot_id");
        if (v) {
            uint32_t start = (uint32_t)sh_json_as_int(v, 0);
            uint32_t end = (uint32_t)sh_json_as_int(sh_json_get(veh, "end_depot_id"), start);
            if (sg_vehicle_set_depots(ctx, id, start, end) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "shift_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(veh, "shift_late"), 0);
            if (sg_vehicle_set_shift_time_window(ctx, id, early, late) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "capacity");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t dim = sh_json_array_len(v);
            double *cap = (double *)malloc(dim * sizeof(double));
            size_t j;
            if (!cap) return -1;
            for (j = 0; j < dim; j++) {
                cap[j] = sh_json_as_double(sh_json_array_get(v, j), 0.0);
            }
            if (sg_vehicle_set_capacity(ctx, id, cap, (uint32_t)dim) != SG_STATUS_OK) {
                free(cap);
                return -1;
            }
            free(cap);
        }

        v = sh_json_get(veh, "initial_load");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t dim = sh_json_array_len(v);
            double *il = (double *)malloc(dim * sizeof(double));
            size_t j;
            if (!il) return -1;
            for (j = 0; j < dim; j++) {
                il[j] = sh_json_as_double(sh_json_array_get(v, j), 0.0);
            }
            if (sg_vehicle_set_initial_load(ctx, id, il, (uint32_t)dim) != SG_STATUS_OK) {
                free(il);
                return -1;
            }
            free(il);
        }

        v = sh_json_get(veh, "qualifications");
        if (v) {
            uint64_t q = (uint64_t)sh_json_as_double(v, 0.0);
            if (sg_vehicle_set_qualifications(ctx, id, q) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "open_end");
        if (v) {
            int oe = sh_json_as_bool(v, false) ? 1 : 0;
            if (sg_vehicle_set_open_end(ctx, id, oe) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "open_start");
        if (v) {
            int os = sh_json_as_bool(v, false) ? 1 : 0;
            if (sg_vehicle_set_open_start(ctx, id, os) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "max_duration");
        if (v) {
            int32_t md = sh_json_as_int(v, 0);
            if (sg_vehicle_set_max_duration(ctx, id, md) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Cost model */
        {
            ShJsonValue *fc = sh_json_get(veh, "fixed_cost");
            ShJsonValue *cpd = sh_json_get(veh, "cost_per_distance");
            ShJsonValue *cpt = sh_json_get(veh, "cost_per_duration");
            if (fc || cpd || cpt) {
                double fixed = fc ? sh_json_as_double(fc, 0.0) : 0.0;
                double dist_c = cpd ? sh_json_as_double(cpd, 1.0) : 1.0;
                double dur_c = cpt ? sh_json_as_double(cpt, 0.0) : 0.0;
                if (sg_vehicle_set_costs(ctx, id, fixed, dist_c, dur_c) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(veh, "cost_per_waiting");
        if (v) {
            if (sg_vehicle_set_waiting_cost(ctx, id, sh_json_as_double(v, 0.0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "cost_per_overtime");
        if (v) {
            if (sg_vehicle_set_overtime_cost(ctx, id, sh_json_as_double(v, 0.0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "depot_loading_seconds");
        if (v) {
            if (sg_vehicle_set_depot_loading_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "depot_unloading_seconds");
        if (v) {
            if (sg_vehicle_set_depot_unloading_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Break policy */
        {
            ShJsonValue *bmw = sh_json_get(veh, "break_max_work_seconds");
            ShJsonValue *bds = sh_json_get(veh, "break_duration_seconds");
            if (bmw || bds) {
                int32_t mw = bmw ? sh_json_as_int(bmw, 0) : 0;
                int32_t bd = bds ? sh_json_as_int(bds, 0) : 0;
                if (sg_vehicle_set_break_policy(ctx, id, mw, bd) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(veh, "max_total_work_seconds");
        if (v) {
            if (sg_vehicle_set_max_total_work(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Multi-trip */
        v = sh_json_get(veh, "max_trips");
        if (v) {
            if (sg_vehicle_set_max_trips(ctx, id, (uint32_t)sh_json_as_int(v, 1)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "trip_reload_seconds");
        if (v) {
            if (sg_vehicle_set_trip_reload_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "max_tasks");
        if (v) {
            if (sg_vehicle_set_max_tasks(ctx, id, (uint32_t)sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "max_distance");
        if (v) {
            if (sg_vehicle_set_max_distance(ctx, id, sh_json_as_double(v, 0.0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "travel_profile_id");
        if (v) {
            uint32_t tp_id = (uint32_t)sh_json_as_int(v, 0);
            if (sg_vehicle_set_travel_profile(ctx, id, tp_id) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* PD stacking policy */
        v = sh_json_get(veh, "pd_policy");
        if (v) {
            const char *pol = sh_json_as_string(v, "none");
            SGPDPolicy policy = SG_PD_POLICY_NONE;
            if (strcmp(pol, "lifo") == 0) policy = SG_PD_POLICY_LIFO;
            else if (strcmp(pol, "fifo") == 0) policy = SG_PD_POLICY_FIFO;
            if (sg_vehicle_set_pd_policy(ctx, id, policy) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Backhaul constraint */
        v = sh_json_get(veh, "backhaul");
        if (v) {
            if (sg_vehicle_set_backhaul(ctx, id, sh_json_as_bool(v, false) ? 1 : 0) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Compartments */
        v = sh_json_get(veh, "compartments");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t clen = sh_json_array_len(v);
            size_t ci;
            for (ci = 0; ci < clen; ci++) {
                ShJsonValue *comp = sh_json_array_get(v, ci);
                uint32_t ctype;
                ShJsonValue *cap_arr;
                if (!comp || sh_json_type(comp) != SH_JSON_OBJECT) return -1;
                ctype = (uint32_t)sh_json_as_int(sh_json_get(comp, "type"), 0);
                cap_arr = sh_json_get(comp, "capacity");
                if (cap_arr && sh_json_type(cap_arr) == SH_JSON_ARRAY) {
                    size_t dim = sh_json_array_len(cap_arr);
                    double *cap = (double *)malloc(dim * sizeof(double));
                    size_t cj;
                    if (!cap) return -1;
                    for (cj = 0; cj < dim; cj++) {
                        cap[cj] = sh_json_as_double(sh_json_array_get(cap_arr, cj), 0.0);
                    }
                    if (sg_vehicle_add_compartment(ctx, id, ctype, cap, (uint32_t)dim) != SG_STATUS_OK) {
                        free(cap);
                        return -1;
                    }
                    free(cap);
                } else {
                    if (sg_vehicle_add_compartment(ctx, id, ctype, NULL, 0) != SG_STATUS_OK) {
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}

static int build_tasks(SGContext *ctx, const ShJsonValue *tasks_arr) {
    size_t i, count;

    if (!tasks_arr || sh_json_type(tasks_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(tasks_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *t = sh_json_array_get(tasks_arr, i);
        const char *type_str;
        SGTaskType type;
        uint32_t id;
        ShJsonValue *v;

        if (!t || sh_json_type(t) != SH_JSON_OBJECT) {
            return -1;
        }

        type_str = sh_json_as_string(sh_json_get(t, "type"), "delivery");
        if (strcmp(type_str, "pickup") == 0) {
            type = SG_TASK_PICKUP;
        } else if (strcmp(type_str, "delivery") == 0) {
            type = SG_TASK_DELIVERY;
        } else if (strcmp(type_str, "service") == 0) {
            type = SG_TASK_SERVICE;
        } else {
            return -1;
        }

        id = sg_add_task(ctx, type);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(t, "x");
        if (v) {
            double x = sh_json_as_double(v, 0.0);
            double y = sh_json_as_double(sh_json_get(t, "y"), 0.0);
            if (sg_task_set_location(ctx, id, x, y) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "location_id");
        if (v) {
            uint32_t loc_id = (uint32_t)sh_json_as_int(v, 0);
            if (sg_task_set_location_id(ctx, id, loc_id) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "tw_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(t, "tw_late"), 0);
            if (sg_task_set_time_window(ctx, id, early, late) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "service_seconds");
        if (v) {
            if (sg_task_set_service_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "demand");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t dim = sh_json_array_len(v);
            double *demand = (double *)malloc(dim * sizeof(double));
            size_t j;
            if (!demand) return -1;
            for (j = 0; j < dim; j++) {
                demand[j] = sh_json_as_double(sh_json_array_get(v, j), 0.0);
            }
            if (sg_task_set_demand(ctx, id, demand, (uint32_t)dim) != SG_STATUS_OK) {
                free(demand);
                return -1;
            }
            free(demand);
        }

        /* Soft time window */
        v = sh_json_get(t, "soft_time_window");
        if (v && sh_json_type(v) == SH_JSON_OBJECT) {
            int32_t se = sh_json_as_int(sh_json_get(v, "early"), 0);
            int32_t sl = sh_json_as_int(sh_json_get(v, "late"), 0);
            double ep = sh_json_as_double(sh_json_get(v, "early_penalty"), 0.0);
            double lp = sh_json_as_double(sh_json_get(v, "late_penalty"), 0.0);
            if (sg_task_set_soft_time_window(ctx, id, se, sl, ep, lp) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Disjunct time windows */
        v = sh_json_get(t, "time_windows");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t twc = sh_json_array_len(v);
            size_t ti;
            for (ti = 0; ti < twc; ti++) {
                ShJsonValue *tw = sh_json_array_get(v, ti);
                if (!tw || sh_json_type(tw) != SH_JSON_OBJECT) return -1;
                int32_t twe = sh_json_as_int(sh_json_get(tw, "early"), 0);
                int32_t twl = sh_json_as_int(sh_json_get(tw, "late"), 0);
                if (sg_task_add_time_window(ctx, id, twe, twl) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int build_requests(SGContext *ctx, const ShJsonValue *requests_arr) {
    size_t i, count;

    if (!requests_arr || sh_json_type(requests_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(requests_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *r = sh_json_array_get(requests_arr, i);
        uint32_t id;
        ShJsonValue *v;

        if (!r || sh_json_type(r) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_request(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(r, "pickup_task_id");
        if (v) {
            uint32_t pickup = (uint32_t)sh_json_as_int(v, 0);
            uint32_t delivery = (uint32_t)sh_json_as_int(sh_json_get(r, "delivery_task_id"), 0);
            if (sg_request_bind_pickup_delivery_tasks(ctx, id, pickup, delivery) !=
                SG_STATUS_OK) {
                return -1;
            }
        } else {
            v = sh_json_get(r, "delivery_task_id");
            if (v) {
                uint32_t delivery = (uint32_t)sh_json_as_int(v, 0);
                if (sg_request_bind_delivery_task(ctx, id, delivery) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "priority");
        if (v) {
            sg_request_set_priority_hint(ctx, id, sh_json_as_int(v, 0));
        }

        v = sh_json_get(r, "zone_id");
        if (v) {
            sg_request_set_zone_hint(ctx, id, (uint32_t)sh_json_as_int(v, 0));
        }

        v = sh_json_get(r, "tw_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(r, "tw_late"), 0);
            sg_request_set_time_window_hint(ctx, id, early, late);
        }

        v = sh_json_get(r, "required_qualifications");
        if (v) {
            uint64_t q = (uint64_t)sh_json_as_double(v, 0.0);
            if (sg_request_set_required_qualifications(ctx, id, q) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "max_ride_time");
        if (v) {
            if (sg_request_set_max_ride_time(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "allowed_vehicles");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t alen = sh_json_array_len(v);
            size_t ai;
            for (ai = 0; ai < alen; ai++) {
                uint32_t vid = (uint32_t)sh_json_as_int(sh_json_array_get(v, ai), 0);
                if (sg_request_add_allowed_vehicle(ctx, id, vid) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "forbidden_vehicles");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t flen = sh_json_array_len(v);
            size_t fi;
            for (fi = 0; fi < flen; fi++) {
                uint32_t vid = (uint32_t)sh_json_as_int(sh_json_array_get(v, fi), 0);
                if (sg_request_add_forbidden_vehicle(ctx, id, vid) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "commodity");
        if (v) {
            uint32_t cid = (uint32_t)sh_json_as_int(v, 0);
            if (sg_request_set_commodity(ctx, id, cid) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "compartment_type");
        if (v) {
            uint32_t ct = (uint32_t)sh_json_as_int(v, 0);
            if (sg_request_set_compartment_type(ctx, id, ct) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "exclusion_groups");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t elen = sh_json_array_len(v);
            size_t ei;
            for (ei = 0; ei < elen; ei++) {
                uint32_t gid = (uint32_t)sh_json_as_int(sh_json_array_get(v, ei), 0);
                if (sg_request_add_exclusion_group(ctx, id, gid) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "setup_class");
        if (v) {
            uint32_t sc = (uint32_t)sh_json_as_int(v, 0);
            if (sg_request_set_setup_class(ctx, id, sc) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "unassigned_penalty");
        if (v) {
            double pen = sh_json_as_double(v, 0.0);
            if (sg_request_set_unassigned_penalty(ctx, id, pen) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_initial_routes(SGContext *ctx, const ShJsonValue *ir_arr) {
    size_t i, count;
    uint32_t *vehicle_ids = NULL;
    uint32_t *route_lengths = NULL;
    uint32_t *request_ids = NULL;
    uint32_t total_requests = 0;
    SGStatus status;

    if (!ir_arr || sh_json_type(ir_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(ir_arr);
    if (count == 0) return 0;

    /* First pass: count total request IDs */
    for (i = 0; i < count; i++) {
        ShJsonValue *route = sh_json_array_get(ir_arr, i);
        ShJsonValue *rids;
        if (!route || sh_json_type(route) != SH_JSON_OBJECT) return -1;
        rids = sh_json_get(route, "request_ids");
        if (!rids || sh_json_type(rids) != SH_JSON_ARRAY) return -1;
        total_requests += (uint32_t)sh_json_array_len(rids);
    }

    vehicle_ids = (uint32_t *)malloc(count * sizeof(uint32_t));
    route_lengths = (uint32_t *)malloc(count * sizeof(uint32_t));
    request_ids = total_requests > 0 ? (uint32_t *)malloc(total_requests * sizeof(uint32_t)) : NULL;
    if (!vehicle_ids || !route_lengths || (total_requests > 0 && !request_ids)) {
        free(vehicle_ids);
        free(route_lengths);
        free(request_ids);
        return -1;
    }

    /* Second pass: fill arrays */
    {
        uint32_t ri = 0;
        for (i = 0; i < count; i++) {
            ShJsonValue *route = sh_json_array_get(ir_arr, i);
            ShJsonValue *rids = sh_json_get(route, "request_ids");
            size_t rlen = sh_json_array_len(rids);
            size_t j;

            vehicle_ids[i] = (uint32_t)sh_json_as_int(sh_json_get(route, "vehicle_id"), 0);
            route_lengths[i] = (uint32_t)rlen;

            for (j = 0; j < rlen; j++) {
                request_ids[ri++] = (uint32_t)sh_json_as_int(sh_json_array_get(rids, j), 0);
            }
        }
    }

    status = sg_set_initial_routes(ctx, (uint32_t)count, vehicle_ids, route_lengths, request_ids);
    free(vehicle_ids);
    free(route_lengths);
    free(request_ids);
    return (status == SG_STATUS_OK) ? 0 : -1;
}

/* ============================================================================
 * sg_api_build_model — build model from parsed JSON DOM
 * ============================================================================ */

SGStatus sg_api_build_model(SGContext *ctx, const ShJsonValue *root) {
    ShJsonValue *v;

    if (!ctx || !root || sh_json_type(root) != SH_JSON_OBJECT) {
        return SG_STATUS_INVALID_ARG;
    }

    /* 1. Scalars */
    v = sh_json_get(root, "dimension_count");
    if (v) {
        int dim = sh_json_as_int(v, 1);
        if (dim > 0 && sg_set_dimension_count(ctx, (uint32_t)dim) != SG_STATUS_OK) {
            return SG_STATUS_INVALID_ARG;
        }
    }

    v = sh_json_get(root, "demand_sign_convention");
    if (v) {
        sg_set_demand_sign_convention(ctx, (SGDemandSignConvention)sh_json_as_int(v, 0));
    }

    v = sh_json_get(root, "unassigned_weight");
    if (v) {
        sg_set_unassigned_weight(ctx, sh_json_as_double(v, 1000000.0));
    }

    v = sh_json_get(root, "span_cost_duration");
    if (v) {
        sg_set_span_cost_duration(ctx, sh_json_as_double(v, 0.0));
    }
    v = sh_json_get(root, "span_cost_distance");
    if (v) {
        sg_set_span_cost_distance(ctx, sh_json_as_double(v, 0.0));
    }

    /* 2. Config */
    if (build_config(ctx, sh_json_get(root, "config")) != SG_STATUS_OK) {
        return SG_STATUS_ERROR;
    }

    /* 3. Locations (before depots/tasks that use location_id) */
    if (build_locations(ctx, sh_json_get(root, "locations")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 4. Compartment types, commodities, exclusion groups, setup times (before vehicles/requests) */
    if (build_compartment_types(ctx, sh_json_get(root, "compartment_types")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_commodities(ctx, sh_json_get(root, "commodities")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_exclusion_groups(ctx, sh_json_get(root, "exclusion_groups")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_setup_times(ctx, sh_json_get(root, "setup_times")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 5. Depots (before vehicles) */
    if (build_depots(ctx, sh_json_get(root, "depots")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 5b. Speed profiles, then travel profiles (before vehicles) */
    if (build_speed_profiles(ctx, sh_json_get(root, "speed_profiles")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_travel_profiles(ctx, sh_json_get(root, "travel_profiles")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 6. Vehicles */
    if (build_vehicles(ctx, sh_json_get(root, "vehicles")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 7. Tasks */
    if (build_tasks(ctx, sh_json_get(root, "tasks")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 8. Requests (references tasks, commodities, exclusion_groups, vehicles) */
    if (build_requests(ctx, sh_json_get(root, "requests")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 9. Travel and zones (after locations) */
    if (build_travel(ctx, sh_json_get(root, "travel")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_zones(ctx, sh_json_get(root, "zones")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 10. Initial routes (references vehicles and requests) */
    if (build_initial_routes(ctx, sh_json_get(root, "initial_routes")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 10b. Precedences (after requests are built) */
    {
        ShJsonValue *prec_arr = sh_json_get(root, "precedences");
        if (prec_arr && sh_json_type(prec_arr) == SH_JSON_ARRAY) {
            size_t pi, pn = sh_json_array_len(prec_arr);
            for (pi = 0; pi < pn; pi++) {
                ShJsonValue *pair = sh_json_array_get(prec_arr, pi);
                uint32_t before_id = (uint32_t)sh_json_as_int(sh_json_get(pair, "before"), 0);
                uint32_t after_id = (uint32_t)sh_json_as_int(sh_json_get(pair, "after"), 0);
                if (sg_add_precedence(ctx, before_id, after_id) != SG_STATUS_OK) {
                    return SG_STATUS_ERROR;
                }
            }
        }
    }

    /* 11. Request locks (after requests and initial_routes) */
    {
        ShJsonValue *committed_arr = sh_json_get(root, "committed_requests");
        ShJsonValue *frozen_arr = sh_json_get(root, "frozen_requests");
        if (committed_arr && sh_json_type(committed_arr) == SH_JSON_ARRAY) {
            size_t i, n = sh_json_array_len(committed_arr);
            for (i = 0; i < n; i++) {
                uint32_t rid = (uint32_t)sh_json_as_int(sh_json_array_get(committed_arr, i), 0);
                if (sg_request_set_lock(ctx, rid, SG_LOCK_COMMITTED) != SG_STATUS_OK) {
                    return SG_STATUS_ERROR;
                }
            }
        }
        if (frozen_arr && sh_json_type(frozen_arr) == SH_JSON_ARRAY) {
            size_t i, n = sh_json_array_len(frozen_arr);
            for (i = 0; i < n; i++) {
                uint32_t rid = (uint32_t)sh_json_as_int(sh_json_array_get(frozen_arr, i), 0);
                if (sg_request_set_lock(ctx, rid, SG_LOCK_FROZEN) != SG_STATUS_OK) {
                    return SG_STATUS_ERROR;
                }
            }
        }
    }

    return SG_STATUS_OK;
}

/* ============================================================================
 * sg_api_build_model_file — read JSON file and build model
 * ============================================================================ */

SGStatus sg_api_build_model_file(SGContext *ctx, const char *path) {
    FILE *f;
    long fsize;
    char *buf;
    size_t nread;
    SHArena *arena;
    ShJsonValue *root;
    ShJsonStatus parse_status;
    SGStatus result;

    if (!ctx || !path) return SG_STATUS_INVALID_ARG;

    f = fopen(path, "rb");
    if (!f) return SG_STATUS_ERROR;

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        return SG_STATUS_INVALID_ARG;
    }

    buf = (char *)malloc((size_t)fsize);
    if (!buf) {
        fclose(f);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    if ((long)nread != fsize) {
        free(buf);
        return SG_STATUS_ERROR;
    }

    arena = sh_arena_create((size_t)fsize * 2 + 4096);
    if (!arena) {
        free(buf);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    parse_status = sh_json_parse(buf, (size_t)fsize, arena, &root);
    free(buf);

    if (parse_status != SH_JSON_OK || !root) {
        sh_arena_free(arena);
        return SG_STATUS_INVALID_ARG;
    }

    result = sg_api_build_model(ctx, root);
    sh_arena_free(arena);
    return result;
}

/* ============================================================================
 * Violation type name mapping
 * ============================================================================ */

static const char *violation_type_name(SGViolationType type) {
    switch (type) {
        case SG_VIOLATION_HARD_TW:           return "hard_tw";
        case SG_VIOLATION_CAPACITY:          return "capacity";
        case SG_VIOLATION_PD_ORDER:          return "pd_order";
        case SG_VIOLATION_RIDE_TIME:         return "ride_time";
        case SG_VIOLATION_MAX_DURATION:      return "max_duration";
        case SG_VIOLATION_MAX_DISTANCE:      return "max_distance";
        case SG_VIOLATION_MAX_TASKS:         return "max_tasks";
        case SG_VIOLATION_FORBIDDEN_VEHICLE: return "forbidden_vehicle";
        case SG_VIOLATION_QUALIFICATION:     return "qualification";
        case SG_VIOLATION_UNKNOWN_TASK:      return "unknown_task";
        case SG_VIOLATION_DUPLICATE_TASK:    return "duplicate_task";
        default:                             return "UNKNOWN";
    }
}

static void write_violations(const SGContext *ctx, ShJsonWriter *w) {
    uint32_t i, count;
    SGViolation v;

    count = sg_get_violation_count(ctx);
    sh_json_write_key(w, "violations");
    sh_json_write_array_start(w);
    for (i = 0; i < count; i++) {
        if (sg_get_violation(ctx, i, &v) == SG_STATUS_OK) {
            sh_json_write_object_start(w);
            sh_json_write_kv_string(w, "type", violation_type_name(v.type));
            if (v.vehicle_id != UINT32_MAX)
                sh_json_write_kv_int(w, "vehicle_id", v.vehicle_id);
            if (v.stop_index != UINT32_MAX)
                sh_json_write_kv_int(w, "stop_index", v.stop_index);
            if (v.task_id != UINT32_MAX)
                sh_json_write_kv_int(w, "task_id", v.task_id);
            if (v.request_id != UINT32_MAX)
                sh_json_write_kv_int(w, "request_id", v.request_id);
            sh_json_write_kv_double_fmt(w, "actual", v.actual, 2);
            sh_json_write_kv_double_fmt(w, "limit", v.limit, 2);
            sh_json_write_object_end(w);
        }
    }
    sh_json_write_array_end(w);
}

/* ============================================================================
 * Plan Parsing
 * ============================================================================ */

static int parse_plan_routes(const ShJsonValue *plan_arr,
                             SGPlanRoute **routes_out, uint32_t *num_routes_out,
                             uint32_t **task_buf_out) {
    size_t count, i;
    SGPlanRoute *routes;
    uint32_t total_tasks = 0;
    uint32_t *task_buf;
    uint32_t offset;

    if (!plan_arr || sh_json_type(plan_arr) != SH_JSON_ARRAY) {
        return -1;
    }

    count = sh_json_array_len(plan_arr);
    if (count == 0) {
        *routes_out = NULL;
        *num_routes_out = 0;
        *task_buf_out = NULL;
        return 0;
    }

    /* First pass: count total tasks */
    for (i = 0; i < count; i++) {
        ShJsonValue *route = sh_json_array_get(plan_arr, i);
        ShJsonValue *tids;
        if (!route || sh_json_type(route) != SH_JSON_OBJECT) return -1;
        tids = sh_json_get(route, "task_ids");
        if (!tids || sh_json_type(tids) != SH_JSON_ARRAY) return -1;
        total_tasks += (uint32_t)sh_json_array_len(tids);
    }

    routes = (SGPlanRoute *)malloc(count * sizeof(SGPlanRoute));
    task_buf = total_tasks > 0
               ? (uint32_t *)malloc((size_t)total_tasks * sizeof(uint32_t))
               : NULL;
    if (!routes || (total_tasks > 0 && !task_buf)) {
        free(routes);
        free(task_buf);
        return -1;
    }

    /* Second pass: fill */
    offset = 0;
    for (i = 0; i < count; i++) {
        ShJsonValue *route = sh_json_array_get(plan_arr, i);
        ShJsonValue *tids = sh_json_get(route, "task_ids");
        size_t tlen = sh_json_array_len(tids);
        size_t j;

        routes[i].vehicle_id = (uint32_t)sh_json_as_int(
            sh_json_get(route, "vehicle_id"), 0);
        routes[i].task_ids = task_buf + offset;
        routes[i].task_count = (uint32_t)tlen;

        for (j = 0; j < tlen; j++) {
            task_buf[offset++] = (uint32_t)sh_json_as_int(
                sh_json_array_get(tids, j), 0);
        }
    }

    *routes_out = routes;
    *num_routes_out = (uint32_t)count;
    *task_buf_out = task_buf;
    return 0;
}

/* ============================================================================
 * sg_api_write_solution — write solution to streaming JSON writer
 * ============================================================================ */

SGStatus sg_api_write_solution(const SGContext *ctx, ShJsonWriter *w,
                                SGStatus solve_status) {
    if (!ctx || !w) return SG_STATUS_INVALID_ARG;

    sh_json_write_object_start(w);

    if (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT) {
        SGStats stats;
        uint32_t route_count, ri, unassigned_count;

        sg_get_stats(ctx, &stats);

        sh_json_write_kv_string(w, "status",
                                solve_status == SG_STATUS_OK ? "OK" : "LIMIT");

        /* Stats */
        sh_json_write_key(w, "stats");
        sh_json_write_object_start(w);
        sh_json_write_kv_int(w, "iterations", stats.iterations);
        sh_json_write_kv_double_fmt(w, "total_cost", stats.total_cost, 2);
        sh_json_write_kv_double_fmt(w, "total_distance", stats.total_distance, 2);
        sh_json_write_kv_int(w, "unassigned", stats.unassigned);
        sh_json_write_kv_int(w, "vehicles_used", stats.vehicles_used);
        sh_json_write_kv_double_fmt(w, "total_waiting", stats.total_waiting, 2);
        sh_json_write_kv_double_fmt(w, "total_overtime", stats.total_overtime, 2);
        sh_json_write_kv_double_fmt(w, "total_tw_penalty", stats.total_tw_penalty, 2);
        sh_json_write_kv_double_fmt(w, "duration_span", stats.duration_span, 2);
        sh_json_write_kv_double_fmt(w, "distance_span", stats.distance_span, 2);
        sh_json_write_kv_double_fmt(w, "elapsed_seconds", stats.elapsed_seconds, 3);
        if (stats.insertion_cache_hits > 0 || stats.insertion_cache_misses > 0) {
            sh_json_write_kv_int(w, "insertion_cache_hits", (int64_t)stats.insertion_cache_hits);
            sh_json_write_kv_int(w, "insertion_cache_misses", (int64_t)stats.insertion_cache_misses);
        }
        {
            const char *phase_str;
            switch (stats.phase) {
                case SG_PHASE_CONSTRUCTION:   phase_str = "construction"; break;
                case SG_PHASE_1_VEHICLE_MIN:  phase_str = "phase1"; break;
                case SG_PHASE_1_5_CRUNCH:     phase_str = "phase1_5"; break;
                case SG_PHASE_2_POLISH:       phase_str = "phase2"; break;
                case SG_PHASE_POSTPROCESS:    phase_str = "postprocess"; break;
                default:                      phase_str = "unknown"; break;
            }
            sh_json_write_kv_string(w, "phase", phase_str);
        }
        sh_json_write_object_end(w);

        /* Per-phase breakdown */
        {
            uint32_t pc = sg_get_phase_count(ctx);
            if (pc > 0) {
                uint32_t pi;
                sh_json_write_key(w, "phases");
                sh_json_write_array_start(w);
                for (pi = 0; pi < pc; pi++) {
                    SGPhaseStats ps;
                    if (sg_get_phase_stats(ctx, pi, &ps) == SG_STATUS_OK) {
                        const char *pn;
                        sh_json_write_object_start(w);
                        switch (ps.phase) {
                            case SG_PHASE_CONSTRUCTION:   pn = "construction"; break;
                            case SG_PHASE_1_VEHICLE_MIN:  pn = "phase1"; break;
                            case SG_PHASE_1_5_CRUNCH:     pn = "phase1_5"; break;
                            case SG_PHASE_2_POLISH:       pn = "phase2"; break;
                            case SG_PHASE_POSTPROCESS:    pn = "postprocess"; break;
                            default:                      pn = "unknown"; break;
                        }
                        sh_json_write_kv_string(w, "phase", pn);
                        sh_json_write_kv_int(w, "iterations", ps.iterations);
                        sh_json_write_kv_double_fmt(w, "elapsed_seconds", ps.elapsed_seconds, 3);
                        sh_json_write_kv_double_fmt(w, "start_cost", ps.start_cost, 2);
                        sh_json_write_kv_double_fmt(w, "end_cost", ps.end_cost, 2);
                        sh_json_write_kv_int(w, "start_vehicles", ps.start_vehicles);
                        sh_json_write_kv_int(w, "end_vehicles", ps.end_vehicles);
                        sh_json_write_kv_int(w, "start_unassigned", ps.start_unassigned);
                        sh_json_write_kv_int(w, "end_unassigned", ps.end_unassigned);
                        sh_json_write_object_end(w);
                    }
                }
                sh_json_write_array_end(w);
            }
        }

        /* Convergence history */
        {
            uint32_t cc = sg_get_convergence_count(ctx);
            SGConvergenceEntry test_entry;
            if (cc > 0 && sg_get_convergence_entry(ctx, 0, &test_entry) == SG_STATUS_OK) {
                uint32_t stored = cc;
                uint32_t ci;
                while (stored > 0 && sg_get_convergence_entry(ctx, stored - 1, &test_entry) != SG_STATUS_OK)
                    stored--;
                sh_json_write_key(w, "convergence");
                sh_json_write_array_start(w);
                for (ci = 0; ci < stored; ci++) {
                    SGConvergenceEntry ce;
                    if (sg_get_convergence_entry(ctx, ci, &ce) == SG_STATUS_OK) {
                        const char *cpn;
                        sh_json_write_object_start(w);
                        sh_json_write_kv_int(w, "iteration", ce.iteration);
                        sh_json_write_kv_double_fmt(w, "cost", ce.cost, 2);
                        sh_json_write_kv_double_fmt(w, "elapsed", ce.elapsed_seconds, 3);
                        switch (ce.phase) {
                            case SG_PHASE_CONSTRUCTION:   cpn = "construction"; break;
                            case SG_PHASE_1_VEHICLE_MIN:  cpn = "phase1"; break;
                            case SG_PHASE_1_5_CRUNCH:     cpn = "phase1_5"; break;
                            case SG_PHASE_2_POLISH:       cpn = "phase2"; break;
                            case SG_PHASE_POSTPROCESS:    cpn = "postprocess"; break;
                            default:                      cpn = "unknown"; break;
                        }
                        sh_json_write_kv_string(w, "phase", cpn);
                        sh_json_write_kv_int(w, "vehicles", ce.vehicles_used);
                        sh_json_write_kv_int(w, "unassigned", ce.unassigned);
                        sh_json_write_kv_bool(w, "is_new_best", ce.is_new_best);
                        sh_json_write_object_end(w);
                    }
                }
                sh_json_write_array_end(w);
            }
        }

        /* Penalty weights */
        {
            SGPenaltySnapshot pen;
            if (sg_get_penalty_snapshot(ctx, &pen) == SG_STATUS_OK) {
                int has_nonzero = 0;
                int pi;
                for (pi = 0; pi < SG_PENALTY_TYPE_COUNT; pi++) {
                    if (pen.weight[pi] > 1e-12) { has_nonzero = 1; break; }
                }
                if (has_nonzero) {
                    sh_json_write_key(w, "penalty_weights");
                    sh_json_write_object_start(w);
                    sh_json_write_kv_double_fmt(w, "time_warp", pen.weight[0], 4);
                    sh_json_write_kv_double_fmt(w, "capacity", pen.weight[1], 4);
                    sh_json_write_kv_double_fmt(w, "duration", pen.weight[2], 4);
                    sh_json_write_kv_double_fmt(w, "ride_time", pen.weight[3], 4);
                    sh_json_write_kv_double_fmt(w, "distance", pen.weight[4], 4);
                    sh_json_write_kv_double_fmt(w, "total_work", pen.weight[5], 4);
                    sh_json_write_object_end(w);
                }
            }
        }

        /* Operator telemetry */
        {
            uint32_t nd = sg_get_destroy_operator_count(ctx);
            uint32_t nr = sg_get_repair_operator_count(ctx);
            if (nd > 0 || nr > 0) {
                uint32_t oi;
                sh_json_write_key(w, "operators");
                sh_json_write_object_start(w);
                if (nd > 0) {
                    sh_json_write_key(w, "destroy");
                    sh_json_write_array_start(w);
                    for (oi = 0; oi < nd; oi++) {
                        SGOperatorStats os;
                        if (sg_get_destroy_operator_stats(ctx, oi, &os) == SG_STATUS_OK) {
                            sh_json_write_object_start(w);
                            sh_json_write_kv_string(w, "name", os.name);
                            sh_json_write_kv_double_fmt(w, "weight", os.weight, 3);
                            sh_json_write_kv_int(w, "selected", os.selected);
                            sh_json_write_kv_int(w, "accepted", os.accepted);
                            sh_json_write_kv_int(w, "improvements", os.improvements);
                            sh_json_write_kv_double_fmt(w, "total_seconds", os.total_seconds, 3);
                            sh_json_write_object_end(w);
                        }
                    }
                    sh_json_write_array_end(w);
                }
                if (nr > 0) {
                    sh_json_write_key(w, "repair");
                    sh_json_write_array_start(w);
                    for (oi = 0; oi < nr; oi++) {
                        SGOperatorStats os;
                        if (sg_get_repair_operator_stats(ctx, oi, &os) == SG_STATUS_OK) {
                            sh_json_write_object_start(w);
                            sh_json_write_kv_string(w, "name", os.name);
                            sh_json_write_kv_double_fmt(w, "weight", os.weight, 3);
                            sh_json_write_kv_int(w, "selected", os.selected);
                            sh_json_write_kv_int(w, "accepted", os.accepted);
                            sh_json_write_kv_int(w, "improvements", os.improvements);
                            sh_json_write_kv_double_fmt(w, "total_seconds", os.total_seconds, 3);
                            sh_json_write_object_end(w);
                        }
                    }
                    sh_json_write_array_end(w);
                }
                sh_json_write_object_end(w);
            }
        }

        /* Routes */
        route_count = sg_solution_get_route_count(ctx);
        sh_json_write_key(w, "routes");
        sh_json_write_array_start(w);

        for (ri = 0; ri < route_count; ri++) {
            uint32_t vehicle_id = sg_solution_get_route_vehicle_id(ctx, ri);
            uint32_t stop_count = sg_solution_get_route_stop_count(ctx, ri);
            uint32_t si;

            sh_json_write_object_start(w);
            sh_json_write_kv_int(w, "vehicle_id", vehicle_id);
            sh_json_write_kv_double_fmt(w, "distance",
                sg_solution_get_route_distance(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "duration",
                sg_solution_get_route_duration(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "waiting",
                sg_solution_get_route_waiting(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "overtime",
                sg_solution_get_route_overtime(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "tw_penalty",
                sg_solution_get_route_tw_penalty(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "break_time",
                sg_solution_get_route_break_time(ctx, ri), 2);
            sh_json_write_kv_int(w, "break_count",
                sg_solution_get_route_break_count(ctx, ri));
            sh_json_write_kv_double_fmt(w, "total_work",
                sg_solution_get_route_total_work(ctx, ri), 2);
            sh_json_write_kv_int(w, "trip_count",
                sg_solution_get_route_trip_count(ctx, ri));

            /* Break position records */
            {
                uint32_t bc = sg_solution_get_route_break_count(ctx, ri);
                if (bc > 0) {
                    uint32_t bi;
                    sh_json_write_key(w, "breaks");
                    sh_json_write_array_start(w);
                    for (bi = 0; bi < bc; bi++) {
                        uint32_t after_stop;
                        double bstart, bdur;
                        if (sg_solution_get_route_break(ctx, ri, bi,
                                &after_stop, &bstart, &bdur) == SG_STATUS_OK) {
                            sh_json_write_object_start(w);
                            sh_json_write_kv_int(w, "after_stop_index", after_stop);
                            sh_json_write_kv_double_fmt(w, "start", bstart, 2);
                            sh_json_write_kv_double_fmt(w, "duration", bdur, 2);
                            sh_json_write_object_end(w);
                        }
                    }
                    sh_json_write_array_end(w);
                }
            }

            sh_json_write_key(w, "stops");
            sh_json_write_array_start(w);

            for (si = 0; si < stop_count; si++) {
                SGSolutionStop stop;
                if (sg_solution_get_route_stop(ctx, ri, si, &stop) == SG_STATUS_OK) {
                    const char *type_str;
                    sh_json_write_object_start(w);
                    sh_json_write_kv_int(w, "request_id", stop.request_id);
                    sh_json_write_kv_int(w, "task_id", stop.task_id);
                    switch (stop.stop_type) {
                        case SG_STOP_TYPE_PICKUP:   type_str = "pickup"; break;
                        case SG_STOP_TYPE_DELIVERY:  type_str = "delivery"; break;
                        case SG_STOP_TYPE_SERVICE:   type_str = "service"; break;
                        default:                     type_str = "unknown"; break;
                    }
                    sh_json_write_kv_string(w, "type", type_str);
                    sh_json_write_kv_double_fmt(w, "arrival", stop.arrival, 2);
                    sh_json_write_kv_double_fmt(w, "service_start", stop.service_start, 2);
                    sh_json_write_kv_double_fmt(w, "departure", stop.departure, 2);
                    sh_json_write_kv_int(w, "trip_index", stop.trip_index);
                    sh_json_write_object_end(w);
                }
            }

            sh_json_write_array_end(w);  /* stops */
            sh_json_write_object_end(w);  /* route */
        }

        sh_json_write_array_end(w);  /* routes */

        /* Unassigned */
        unassigned_count = sg_get_unassigned(ctx);
        sh_json_write_key(w, "unassigned");
        sh_json_write_array_start(w);
        for (ri = 0; ri < unassigned_count; ri++) {
            sh_json_write_int(w, sg_solution_get_unassigned_request(ctx, ri));
        }
        sh_json_write_array_end(w);
    } else if (solve_status == SG_STATUS_INFEASIBLE) {
        sh_json_write_kv_string(w, "status", "INFEASIBLE");
    } else {
        sh_json_write_kv_string(w, "status", "ERROR");
    }

    /* Error string (always present, empty on success) */
    {
        const char *err = sg_get_last_error(ctx);
        sh_json_write_kv_string(w, "error", err ? err : "");
    }

    sh_json_write_object_end(w);

    return sh_json_writer_error(w) ? SG_STATUS_ERROR : SG_STATUS_OK;
}

/* ============================================================================
 * Solve Handler (refactored to use build_model + write_solution)
 * ============================================================================ */

char *sg_api_solve(const char *json_body, size_t body_len,
                   int *status_code, size_t *out_len) {
    SHArena *arena = NULL;
    ShJsonValue *root = NULL;
    ShJsonStatus parse_status;
    SGContext *ctx = NULL;
    SGStatus build_status, solve_status;
    ShJsonBuf jb;
    ShJsonWriter w;
    char *result;

    if (!json_body || body_len == 0) {
        if (status_code) *status_code = 400;
        return make_error_json(400, "empty request body", out_len);
    }

    arena = sh_arena_create(body_len * 2 + 4096);
    if (!arena) {
        if (status_code) *status_code = 500;
        return make_error_json(500, "out of memory", out_len);
    }

    parse_status = sh_json_parse(json_body, body_len, arena, &root);
    if (parse_status != SH_JSON_OK || !root || sh_json_type(root) != SH_JSON_OBJECT) {
        sh_arena_free(arena);
        if (status_code) *status_code = 400;
        return make_error_json(400, "invalid JSON", out_len);
    }

    ctx = sg_create();
    if (!ctx) {
        sh_arena_free(arena);
        if (status_code) *status_code = 500;
        return make_error_json(500, "failed to create solver context", out_len);
    }

    build_status = sg_api_build_model(ctx, root);

    if (build_status != SG_STATUS_OK) {
        sh_arena_free(arena);
        {
            const char *err = sg_get_last_error(ctx);
            char *resp;
            if (err && err[0]) {
                resp = make_error_json(400, err, out_len);
            } else {
                resp = make_error_json(400, "failed to build model", out_len);
            }
            sg_free(ctx);
            if (status_code) *status_code = 400;
            return resp;
        }
    }

    /* Check for plan validation mode */
    {
        ShJsonValue *plan_arr = sh_json_get(root, "plan");
        if (plan_arr && sh_json_type(plan_arr) == SH_JSON_ARRAY) {
            SGPlanRoute *plan_routes = NULL;
            uint32_t num_plan_routes = 0;
            uint32_t *task_buf = NULL;

            if (parse_plan_routes(plan_arr, &plan_routes, &num_plan_routes,
                                  &task_buf) != 0) {
                sh_arena_free(arena);
                sg_free(ctx);
                if (status_code) *status_code = 400;
                return make_error_json(400, "invalid plan format", out_len);
            }

            sh_arena_free(arena);
            arena = NULL;

            solve_status = sg_validate_plan(ctx, num_plan_routes, plan_routes);
            free(plan_routes);
            free(task_buf);

            sh_json_buf_init(&jb);
            sh_json_writer_init(&w, sh_json_buf_write, &jb);

            sg_api_write_solution(ctx, &w, solve_status);

            /* Rewind: remove trailing '}' to append violations */
            if (!sh_json_writer_error(&w) && jb.len > 0) {
                /* Insert violations before the closing brace.
                   sg_api_write_solution wrote a complete object. We need to
                   splice the violations array into it. Instead, we write
                   a new response with violations included. */
            }

            /* Actually, just build a fresh response with violations */
            sh_json_buf_free(&jb);
            sh_json_buf_init(&jb);
            sh_json_writer_init(&w, sh_json_buf_write, &jb);

            /* Write response with violations */
            sh_json_write_object_start(&w);
            if (solve_status == SG_STATUS_OK) {
                SGStats stats;
                uint32_t route_count, ri, unassigned_count;

                sg_get_stats(ctx, &stats);
                sh_json_write_kv_string(&w, "status", "OK");

                sh_json_write_key(&w, "stats");
                sh_json_write_object_start(&w);
                sh_json_write_kv_int(&w, "iterations", stats.iterations);
                sh_json_write_kv_double_fmt(&w, "total_cost", stats.total_cost, 2);
                sh_json_write_kv_double_fmt(&w, "total_distance", stats.total_distance, 2);
                sh_json_write_kv_int(&w, "unassigned", stats.unassigned);
                sh_json_write_kv_int(&w, "vehicles_used", stats.vehicles_used);
                sh_json_write_kv_double_fmt(&w, "total_waiting", stats.total_waiting, 2);
                sh_json_write_kv_double_fmt(&w, "total_overtime", stats.total_overtime, 2);
                sh_json_write_kv_double_fmt(&w, "total_tw_penalty", stats.total_tw_penalty, 2);
                sh_json_write_kv_double_fmt(&w, "duration_span", stats.duration_span, 2);
                sh_json_write_kv_double_fmt(&w, "distance_span", stats.distance_span, 2);
                sh_json_write_object_end(&w);

                route_count = sg_solution_get_route_count(ctx);
                sh_json_write_key(&w, "routes");
                sh_json_write_array_start(&w);
                for (ri = 0; ri < route_count; ri++) {
                    uint32_t vehicle_id = sg_solution_get_route_vehicle_id(ctx, ri);
                    uint32_t stop_count = sg_solution_get_route_stop_count(ctx, ri);
                    uint32_t si;

                    sh_json_write_object_start(&w);
                    sh_json_write_kv_int(&w, "vehicle_id", vehicle_id);
                    sh_json_write_kv_double_fmt(&w, "distance",
                        sg_solution_get_route_distance(ctx, ri), 2);
                    sh_json_write_kv_double_fmt(&w, "duration",
                        sg_solution_get_route_duration(ctx, ri), 2);

                    sh_json_write_key(&w, "stops");
                    sh_json_write_array_start(&w);
                    for (si = 0; si < stop_count; si++) {
                        SGSolutionStop stop;
                        if (sg_solution_get_route_stop(ctx, ri, si, &stop) == SG_STATUS_OK) {
                            const char *type_str;
                            sh_json_write_object_start(&w);
                            sh_json_write_kv_int(&w, "request_id", stop.request_id);
                            sh_json_write_kv_int(&w, "task_id", stop.task_id);
                            switch (stop.stop_type) {
                                case SG_STOP_TYPE_PICKUP:  type_str = "pickup"; break;
                                case SG_STOP_TYPE_DELIVERY: type_str = "delivery"; break;
                                case SG_STOP_TYPE_SERVICE:  type_str = "service"; break;
                                default:                    type_str = "unknown"; break;
                            }
                            sh_json_write_kv_string(&w, "type", type_str);
                            sh_json_write_kv_double_fmt(&w, "arrival", stop.arrival, 2);
                            sh_json_write_kv_double_fmt(&w, "service_start", stop.service_start, 2);
                            sh_json_write_kv_double_fmt(&w, "departure", stop.departure, 2);
                            sh_json_write_object_end(&w);
                        }
                    }
                    sh_json_write_array_end(&w);
                    sh_json_write_object_end(&w);
                }
                sh_json_write_array_end(&w);

                unassigned_count = sg_get_unassigned(ctx);
                sh_json_write_key(&w, "unassigned");
                sh_json_write_array_start(&w);
                for (ri = 0; ri < unassigned_count; ri++) {
                    sh_json_write_int(&w,
                        sg_solution_get_unassigned_request(ctx, ri));
                }
                sh_json_write_array_end(&w);
            } else {
                sh_json_write_kv_string(&w, "status", "ERROR");
            }

            /* Violations array */
            write_violations(ctx, &w);

            {
                const char *err = sg_get_last_error(ctx);
                sh_json_write_kv_string(&w, "error", err ? err : "");
            }
            sh_json_write_object_end(&w);

            sg_free(ctx);

            if (sh_json_writer_error(&w) || !jb.buf) {
                sh_json_buf_free(&jb);
                if (status_code) *status_code = 500;
                return make_error_json(500, "failed to write response JSON", out_len);
            }

            if (status_code) *status_code = 200;
            if (out_len) *out_len = jb.len;
            return sh_json_buf_take(&jb);
        }
    }

    sh_arena_free(arena);
    arena = NULL;

    if (sg_validate_model(ctx) != SG_STATUS_OK) {
        const char *err = sg_get_last_error(ctx);
        char *resp;
        if (err && err[0]) {
            resp = make_error_json(400, err, out_len);
        } else {
            resp = make_error_json(400, "model validation failed", out_len);
        }
        sg_free(ctx);
        if (status_code) *status_code = 400;
        return resp;
    }

    solve_status = sg_solve(ctx);

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sg_api_write_solution(ctx, &w, solve_status);

    sg_free(ctx);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        if (status_code) *status_code = 500;
        return make_error_json(500, "failed to write response JSON", out_len);
    }

    if (status_code) {
        *status_code = (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT)
                       ? 200 : 400;
    }
    if (out_len) {
        *out_len = jb.len;
    }

    result = sh_json_buf_take(&jb);
    return result;
}

/* ============================================================================
 * Health / Version Handlers
 * ============================================================================ */

char *sg_api_health(size_t *out_len) {
    ShJsonBuf jb;
    ShJsonWriter w;

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "status", "healthy");
    sh_json_write_kv_string(&w, "service", "surge");
    sh_json_write_kv_string(&w, "version", sg_version());
    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NULL;
    }

    if (out_len) {
        *out_len = jb.len;
    }
    return sh_json_buf_take(&jb);
}

char *sg_api_version(size_t *out_len) {
    ShJsonBuf jb;
    ShJsonWriter w;

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "version", sg_version());
    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NULL;
    }

    if (out_len) {
        *out_len = jb.len;
    }
    return sh_json_buf_take(&jb);
}

/* ============================================================================
 * Stats Handler
 * ============================================================================ */

static char *sg_api_stats(size_t *out_len) {
    ShJsonBuf jb;
    ShJsonWriter w;

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "service", "surge");
    sh_json_write_kv_string(&w, "version", sg_version());
    sh_json_write_kv_string(&w, "status", "OK");
    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NULL;
    }

    if (out_len) {
        *out_len = jb.len;
    }
    return sh_json_buf_take(&jb);
}

/* ============================================================================
 * Router
 * ============================================================================ */

int sg_api_handle(void *ctx_, const ShApiRequest *req, ShApiResponse *resp) {
    SGAPIContext *ctx = (SGAPIContext *)ctx_;
    (void)ctx;
    (void)ctx;

    if (!req || !resp || !req->path) {
        return -1;
    }

    memset(resp, 0, sizeof(*resp));
    resp->content_type = "application/json";

    if (strcmp(req->path, "/api/v1/solve") == 0) {
        int code = 200;
        resp->body = (uint8_t *)sg_api_solve(req->body, req->body_len,
                                             &code, &resp->body_len);
        resp->status_code = code;
    } else if (strcmp(req->path, "/api/v1/health") == 0) {
        resp->body = (uint8_t *)sg_api_health(&resp->body_len);
        resp->status_code = resp->body ? 200 : 500;
    } else if (strcmp(req->path, "/api/v1/version") == 0) {
        resp->body = (uint8_t *)sg_api_version(&resp->body_len);
        resp->status_code = resp->body ? 200 : 500;
    } else if (strcmp(req->path, "/api/v1/stats") == 0) {
        resp->body = (uint8_t *)sg_api_stats(&resp->body_len);
        resp->status_code = resp->body ? 200 : 500;
    } else {
        resp->body = (uint8_t *)make_error_json(404, "not found",
                                                &resp->body_len);
        resp->status_code = 404;
    }

    return resp->body ? 0 : -1;
}

/* Response bodies are freed by sh_api_response_free() in shared/src/sh_api.c.
 * Surge used to carry its own identical copy. */
