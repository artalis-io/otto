/*
 * sg_gen_solomon.c - Solomon-format VRPTW instance generator
 *
 * Generates synthetic VRPTW benchmark instances in standard Solomon format
 * with configurable spatial distribution (Clustered, Random, Mixed) and
 * time-window characteristics (Type 1 = narrow, Type 2 = wide).
 *
 * Standalone program: no Surge dependency, only outputs text files.
 *
 * Build:
 *   cc -std=c11 -O2 sg_gen_solomon.c -lm -o sg_gen_solomon
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- xorshift64 RNG ---- */

typedef struct {
    uint64_t state;
} RNG;

static void rng_seed(RNG *rng, uint64_t seed) {
    rng->state = seed ? seed : 1;
}

static uint64_t rng_next(RNG *rng) {
    uint64_t s = rng->state;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    rng->state = s;
    return s;
}

/* Uniform integer in [lo, hi] inclusive */
static int rng_int(RNG *rng, int lo, int hi) {
    if (lo >= hi) return lo;
    uint64_t range = (uint64_t)(hi - lo + 1);
    return lo + (int)(rng_next(rng) % range);
}

/* Uniform double in [0, 1) */
static double rng_double(RNG *rng) {
    return (double)(rng_next(rng) & 0xFFFFFFFFFFFFFULL) / (double)(1ULL << 52);
}

/* Box-Muller: standard normal */
static double rng_normal(RNG *rng) {
    double u1 = rng_double(rng);
    double u2 = rng_double(rng);
    /* Avoid log(0) */
    if (u1 < 1e-15) u1 = 1e-15;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ---- Spatial classes ---- */

enum SpatialClass {
    SPATIAL_CLUSTERED,
    SPATIAL_RANDOM,
    SPATIAL_MIXED
};

/* ---- Time-window types ---- */

enum TWType {
    TW_NARROW,  /* Type 1: many vehicles, tight windows */
    TW_WIDE     /* Type 2: few vehicles, wide windows */
};

/* ---- Instance class ---- */

typedef struct {
    const char *name;       /* e.g. "C1" */
    enum SpatialClass spatial;
    enum TWType tw_type;
} InstanceClass;

static const InstanceClass ALL_CLASSES[] = {
    { "C1",  SPATIAL_CLUSTERED, TW_NARROW },
    { "C2",  SPATIAL_CLUSTERED, TW_WIDE   },
    { "R1",  SPATIAL_RANDOM,    TW_NARROW },
    { "R2",  SPATIAL_RANDOM,    TW_WIDE   },
    { "RC1", SPATIAL_MIXED,     TW_NARROW },
    { "RC2", SPATIAL_MIXED,     TW_WIDE   },
};
#define NUM_CLASSES 6

/* ---- Customer data ---- */

typedef struct {
    int id;
    int x, y;
    int demand;
    int ready_time;
    int due_date;
    int service_time;
} Customer;

/* ---- Coordinate generation ---- */

static int clamp_coord(double v) {
    int c = (int)round(v);
    if (c < 0)   c = 0;
    if (c > 100) c = 100;
    return c;
}

static void gen_clustered(RNG *rng, int n, Customer *custs) {
    int num_clusters = rng_int(rng, 5, 10);
    double cx[10], cy[10];
    int i;

    for (i = 0; i < num_clusters; i++) {
        cx[i] = rng_double(rng) * 100.0;
        cy[i] = rng_double(rng) * 100.0;
    }

    for (i = 0; i < n; i++) {
        int c = rng_int(rng, 0, num_clusters - 1);
        double sigma = 10.0;
        custs[i].x = clamp_coord(cx[c] + rng_normal(rng) * sigma);
        custs[i].y = clamp_coord(cy[c] + rng_normal(rng) * sigma);
    }
}

static void gen_random(RNG *rng, int n, Customer *custs) {
    int i;
    for (i = 0; i < n; i++) {
        custs[i].x = rng_int(rng, 0, 100);
        custs[i].y = rng_int(rng, 0, 100);
    }
}

static void gen_mixed(RNG *rng, int n, Customer *custs) {
    int clustered_count = n / 2;
    int random_count = n - clustered_count;

    gen_clustered(rng, clustered_count, custs);
    gen_random(rng, random_count, custs + clustered_count);
}

/* ---- Distance ---- */

static double euclidean(int x1, int y1, int x2, int y2) {
    double dx = (double)(x1 - x2);
    double dy = (double)(y1 - y2);
    return sqrt(dx * dx + dy * dy);
}

/* ---- Time window generation ---- */

static void gen_time_windows(RNG *rng, const InstanceClass *cls,
                             int n, Customer *custs, int depot_x, int depot_y) {
    int horizon, tw_min, tw_max, capacity;
    int i;

    if (cls->tw_type == TW_NARROW) {
        horizon  = 250;
        tw_min   = 10;
        tw_max   = 50;
        capacity = 200;
    } else {
        horizon  = 1000;
        tw_min   = 50;
        tw_max   = 200;
        capacity = 700;
    }

    /* Depot (stored at index -1, handled by caller) */
    (void)capacity;

    for (i = 0; i < n; i++) {
        double dist = euclidean(depot_x, depot_y, custs[i].x, custs[i].y);
        int earliest_possible = (int)ceil(dist);
        int latest_possible = horizon - (int)ceil(dist);

        if (latest_possible < earliest_possible + tw_min) {
            latest_possible = earliest_possible + tw_min;
        }

        int tw_width = rng_int(rng, tw_min, tw_max);
        int ready = rng_int(rng, earliest_possible,
                            latest_possible > earliest_possible
                                ? latest_possible - tw_width
                                : earliest_possible);
        if (ready < 0) ready = 0;

        int due = ready + tw_width;

        custs[i].ready_time = ready;
        custs[i].due_date = due;
        custs[i].demand = rng_int(rng, 5, 40);
        custs[i].service_time = rng_int(rng, 5, 20);
    }
}

/* ---- Instance generation ---- */

static void generate_instance(const InstanceClass *cls, int size,
                              uint64_t seed, const char *name,
                              const char *output_dir) {
    RNG rng;
    Customer *custs;
    int num_vehicles, capacity, horizon;
    char path[1024];
    FILE *fp;
    int i;

    rng_seed(&rng, seed);

    custs = (Customer *)calloc((size_t)size, sizeof(Customer));
    if (!custs) {
        fprintf(stderr, "Error: out of memory for %d customers\n", size);
        return;
    }

    /* Generate coordinates */
    switch (cls->spatial) {
        case SPATIAL_CLUSTERED: gen_clustered(&rng, size, custs); break;
        case SPATIAL_RANDOM:    gen_random(&rng, size, custs);    break;
        case SPATIAL_MIXED:     gen_mixed(&rng, size, custs);     break;
    }

    /* Set type-dependent parameters */
    if (cls->tw_type == TW_NARROW) {
        horizon      = 250;
        capacity     = 200;
        num_vehicles = (size + 3) / 4;  /* ~25% fill per vehicle */
    } else {
        horizon      = 1000;
        capacity     = 700;
        num_vehicles = (size + 9) / 10; /* ~10% fill per vehicle */
    }

    /* Generate time windows and demands */
    gen_time_windows(&rng, cls, size, custs, 50, 50);

    /* Assign IDs */
    for (i = 0; i < size; i++) {
        custs[i].id = i + 1;
    }

    /* Write output */
    snprintf(path, sizeof(path), "%s/%s.txt", output_dir, name);
    fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "Error: cannot open %s for writing\n", path);
        free(custs);
        return;
    }

    fprintf(fp, "%s\n", name);
    fprintf(fp, "\n");
    fprintf(fp, "VEHICLE\n");
    fprintf(fp, "NUMBER     CAPACITY\n");
    fprintf(fp, "  %d         %d\n", num_vehicles, capacity);
    fprintf(fp, "\n");
    fprintf(fp, "CUSTOMER\n");
    fprintf(fp, "CUST_NO.  XCOORD.  YCOORD.  DEMAND  READY_TIME  DUE_DATE  SERVICE_TIME\n");
    fprintf(fp, " \n");

    /* Depot */
    fprintf(fp, "    0      50       50          0          0      %4d          0\n",
            horizon);

    /* Customers */
    for (i = 0; i < size; i++) {
        fprintf(fp, "%5d    %3d      %3d        %3d      %5d     %5d        %3d\n",
                custs[i].id, custs[i].x, custs[i].y,
                custs[i].demand, custs[i].ready_time,
                custs[i].due_date, custs[i].service_time);
    }

    fclose(fp);
    free(custs);
}

/* ---- Deterministic seed per (class, size, instance_index) ---- */

static uint64_t instance_seed(uint64_t base_seed, int class_idx,
                              int size, int instance_idx) {
    uint64_t s = base_seed;
    s ^= (uint64_t)class_idx * 0x9E3779B97F4A7C15ULL;
    s ^= (uint64_t)size * 0x517CC1B727220A95ULL;
    s ^= (uint64_t)instance_idx * 0x6C62272E07BB0142ULL;
    /* Mix bits */
    s ^= s >> 30;
    s *= 0xBF58476D1CE4E5B9ULL;
    s ^= s >> 27;
    s *= 0x94D049BB133111EBULL;
    s ^= s >> 31;
    return s ? s : 1;
}

/* ---- Directory creation (recursive) ---- */

static void mkdirs(const char *path) {
    char tmp[1024];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ---- CLI ---- */

static void print_usage(const char *argv0) {
    printf("Usage: %s [options]\n", argv0);
    printf("\n");
    printf("Generate Solomon-format VRPTW benchmark instances.\n");
    printf("\n");
    printf("Options:\n");
    printf("  --size <n>        Number of customers (default: 100)\n");
    printf("  --class <name>    C1, C2, R1, R2, RC1, RC2, or ALL (default: ALL)\n");
    printf("  --count <n>       Instances per class (default: 5)\n");
    printf("  --seed <n>        Base seed (default: 42)\n");
    printf("  --output-dir <p>  Output directory (default: benchmarks/generated/vrptw)\n");
    printf("  --help            Show this help\n");
}

static int find_class_index(const char *name) {
    int i;
    for (i = 0; i < NUM_CLASSES; i++) {
        /* Case-insensitive compare */
        const char *a = ALL_CLASSES[i].name;
        const char *b = name;
        int match = 1;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) { match = 0; break; }
            a++; b++;
        }
        if (match && *a == '\0' && *b == '\0') return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    int size = 100;
    int count = 5;
    uint64_t base_seed = 42;
    const char *output_dir = "benchmarks/generated/vrptw";
    int class_filter = -1; /* -1 = ALL */
    int total_generated = 0;
    int i, ci;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = (int)strtol(argv[++i], NULL, 10);
            if (size < 1 || size > 10000) {
                fprintf(stderr, "Error: size must be in [1, 10000]\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--class") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "ALL") == 0 || strcmp(argv[i], "all") == 0) {
                class_filter = -1;
            } else {
                class_filter = find_class_index(argv[i]);
                if (class_filter < 0) {
                    fprintf(stderr, "Error: unknown class '%s'\n", argv[i]);
                    fprintf(stderr, "Valid classes: C1, C2, R1, R2, RC1, RC2, ALL\n");
                    return 1;
                }
            }
            continue;
        }
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = (int)strtol(argv[++i], NULL, 10);
            if (count < 1 || count > 100) {
                fprintf(stderr, "Error: count must be in [1, 100]\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            base_seed = (uint64_t)strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
            continue;
        }
        fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    printf("sg_gen_solomon: size=%d count=%d seed=%llu output=%s\n",
           size, count, (unsigned long long)base_seed, output_dir);

    for (ci = 0; ci < NUM_CLASSES; ci++) {
        int inst;

        if (class_filter >= 0 && ci != class_filter) continue;

        mkdirs(output_dir);

        for (inst = 0; inst < count; inst++) {
            char name[64];
            uint64_t seed = instance_seed(base_seed, ci, size, inst);

            snprintf(name, sizeof(name), "%s_%03d_%02d",
                     ALL_CLASSES[ci].name, size, inst + 1);

            printf("  Generating %s ...\n", name);
            generate_instance(&ALL_CLASSES[ci], size, seed, name, output_dir);
            total_generated++;
        }
    }

    printf("Done: %d instances generated in %s/\n", total_generated, output_dir);
    return 0;
}
