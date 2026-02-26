/*
 * sg_gen_li_lim.c - Li & Lim-format PDPTW instance generator
 *
 * Generates synthetic PDPTW benchmark instances in standard Li & Lim format
 * with configurable spatial distribution (Clustered, Random, Mixed) and
 * time-window characteristics (Type 1 = narrow, Type 2 = wide).
 *
 * Each request has a paired pickup and delivery node. Pickup nodes appear
 * first (IDs 1..n), delivery nodes follow (IDs n+1..2n), and the depot
 * is node 0.
 *
 * Standalone program: no Surge dependency, only outputs text files.
 *
 * Build:
 *   cc -std=c11 -O2 sg_gen_li_lim.c -lm -o sg_gen_li_lim
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
    TW_NARROW,
    TW_WIDE
};

/* ---- Instance class ---- */

typedef struct {
    const char *name;
    enum SpatialClass spatial;
    enum TWType tw_type;
} InstanceClass;

static const InstanceClass ALL_CLASSES[] = {
    { "LC1",  SPATIAL_CLUSTERED, TW_NARROW },
    { "LC2",  SPATIAL_CLUSTERED, TW_WIDE   },
    { "LR1",  SPATIAL_RANDOM,    TW_NARROW },
    { "LR2",  SPATIAL_RANDOM,    TW_WIDE   },
    { "LRC1", SPATIAL_MIXED,     TW_NARROW },
    { "LRC2", SPATIAL_MIXED,     TW_WIDE   },
};
#define NUM_CLASSES 6

/* ---- Node data ---- */

typedef struct {
    int id;
    int x, y;
    int demand;       /* positive for pickup, negative for delivery */
    int tw_early;
    int tw_late;
    int service_time;
    int pickup_id;    /* 0 for pickup nodes, source pickup_id for deliveries */
    int delivery_id;  /* paired delivery_id for pickups, 0 for deliveries */
} PDNode;

/* ---- Coordinate generation ---- */

static int clamp_coord(double v) {
    int c = (int)round(v);
    if (c < 0)   c = 0;
    if (c > 100) c = 100;
    return c;
}

static void gen_clustered_coords(RNG *rng, int n, int *xs, int *ys) {
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
        xs[i] = clamp_coord(cx[c] + rng_normal(rng) * sigma);
        ys[i] = clamp_coord(cy[c] + rng_normal(rng) * sigma);
    }
}

static void gen_random_coords(RNG *rng, int n, int *xs, int *ys) {
    int i;
    for (i = 0; i < n; i++) {
        xs[i] = rng_int(rng, 0, 100);
        ys[i] = rng_int(rng, 0, 100);
    }
}

static void gen_mixed_coords(RNG *rng, int n, int *xs, int *ys) {
    int clustered_count = n / 2;
    int random_count = n - clustered_count;

    gen_clustered_coords(rng, clustered_count, xs, ys);
    gen_random_coords(rng, random_count, xs + clustered_count, ys + clustered_count);
}

/* ---- Distance ---- */

static double euclidean(int x1, int y1, int x2, int y2) {
    double dx = (double)(x1 - x2);
    double dy = (double)(y1 - y2);
    return sqrt(dx * dx + dy * dy);
}

/* ---- Delivery location generation ---- */

static void gen_delivery_location(RNG *rng, int px, int py,
                                  int *dx, int *dy) {
    /* Delivery within distance [30, 50] from pickup at random angle */
    double angle = rng_double(rng) * 2.0 * M_PI;
    double dist = 30.0 + rng_double(rng) * 20.0;
    *dx = clamp_coord((double)px + cos(angle) * dist);
    *dy = clamp_coord((double)py + sin(angle) * dist);
}

/* ---- Instance generation ---- */

static void generate_instance(const InstanceClass *cls, int n_requests,
                              uint64_t seed, const char *name,
                              const char *output_dir) {
    RNG rng;
    int *px, *py;      /* pickup coordinates */
    PDNode *nodes;     /* depot + pickups + deliveries = 2*n+1 nodes */
    int total_nodes;
    int num_vehicles, capacity, horizon;
    int ride_time_slack;
    char path[1024];
    FILE *fp;
    int i;

    rng_seed(&rng, seed);

    total_nodes = 2 * n_requests + 1;

    px = (int *)calloc((size_t)n_requests, sizeof(int));
    py = (int *)calloc((size_t)n_requests, sizeof(int));
    nodes = (PDNode *)calloc((size_t)total_nodes, sizeof(PDNode));

    if (!px || !py || !nodes) {
        fprintf(stderr, "Error: out of memory for %d requests\n", n_requests);
        free(px); free(py); free(nodes);
        return;
    }

    /* Generate pickup coordinates */
    switch (cls->spatial) {
        case SPATIAL_CLUSTERED: gen_clustered_coords(&rng, n_requests, px, py); break;
        case SPATIAL_RANDOM:    gen_random_coords(&rng, n_requests, px, py);    break;
        case SPATIAL_MIXED:     gen_mixed_coords(&rng, n_requests, px, py);     break;
    }

    /* Set type-dependent parameters */
    if (cls->tw_type == TW_NARROW) {
        horizon         = 250;
        capacity        = 200;
        num_vehicles    = (n_requests + 3) / 4;
        ride_time_slack = 30;
    } else {
        horizon         = 1000;
        capacity        = 700;
        num_vehicles    = (n_requests + 9) / 10;
        ride_time_slack = 100;
    }

    /* Depot: node 0 */
    nodes[0].id           = 0;
    nodes[0].x            = 50;
    nodes[0].y            = 50;
    nodes[0].demand       = 0;
    nodes[0].tw_early     = 0;
    nodes[0].tw_late      = horizon;
    nodes[0].service_time = 0;
    nodes[0].pickup_id    = 0;
    nodes[0].delivery_id  = 0;

    /* Generate pickup/delivery pairs */
    for (i = 0; i < n_requests; i++) {
        int pickup_idx   = i + 1;             /* nodes[1..n] */
        int delivery_idx = n_requests + i + 1; /* nodes[n+1..2n] */
        int dx, dy;
        double travel_pd;
        int demand;
        int svc_pickup, svc_delivery;
        int p_early, p_late, tw_width_p;
        int d_early, d_late;

        /* Pickup node */
        nodes[pickup_idx].id = pickup_idx;
        nodes[pickup_idx].x  = px[i];
        nodes[pickup_idx].y  = py[i];

        /* Delivery location: nearby the pickup */
        gen_delivery_location(&rng, px[i], py[i], &dx, &dy);
        nodes[delivery_idx].id = delivery_idx;
        nodes[delivery_idx].x  = dx;
        nodes[delivery_idx].y  = dy;

        /* Demand: positive for pickup, negative for delivery */
        demand = rng_int(&rng, 5, 40);
        nodes[pickup_idx].demand   = demand;
        nodes[delivery_idx].demand = -demand;

        /* Service times */
        svc_pickup   = rng_int(&rng, 5, 20);
        svc_delivery = rng_int(&rng, 5, 20);
        nodes[pickup_idx].service_time   = svc_pickup;
        nodes[delivery_idx].service_time = svc_delivery;

        /* Travel time between pickup and delivery (Euclidean, speed=1) */
        travel_pd = euclidean(px[i], py[i], dx, dy);

        /* Pickup time window */
        {
            double dist_depot_p = euclidean(50, 50, px[i], py[i]);
            int earliest_possible = (int)ceil(dist_depot_p);
            int tw_min, tw_max;

            if (cls->tw_type == TW_NARROW) {
                tw_min = 10;
                tw_max = 50;
            } else {
                tw_min = 50;
                tw_max = 200;
            }

            tw_width_p = rng_int(&rng, tw_min, tw_max);

            /* Ensure ready_time allows arrival from depot */
            {
                int latest_start = horizon - (int)ceil(travel_pd)
                                   - svc_pickup - svc_delivery
                                   - (int)ceil(euclidean(dx, dy, 50, 50));
                if (latest_start < earliest_possible)
                    latest_start = earliest_possible;

                int upper = latest_start - tw_width_p;
                if (upper < earliest_possible) upper = earliest_possible;

                p_early = rng_int(&rng, earliest_possible, upper);
                if (p_early < 0) p_early = 0;
            }

            p_late = p_early + tw_width_p;
        }

        nodes[pickup_idx].tw_early = p_early;
        nodes[pickup_idx].tw_late  = p_late;

        /* Delivery time window: must be consistent with pickup */
        d_early = p_early + svc_pickup + (int)ceil(travel_pd);
        d_late  = p_late + svc_pickup + (int)ceil(travel_pd) + ride_time_slack;

        /* Clamp to horizon */
        if (d_late > horizon) d_late = horizon;
        if (d_early > d_late) d_early = d_late;

        nodes[delivery_idx].tw_early = d_early;
        nodes[delivery_idx].tw_late  = d_late;

        /* Cross-references */
        nodes[pickup_idx].pickup_id    = 0;           /* pickup: PICKUP=0 */
        nodes[pickup_idx].delivery_id  = delivery_idx; /* pickup: DELIVERY=n+i */
        nodes[delivery_idx].pickup_id  = pickup_idx;   /* delivery: PICKUP=i */
        nodes[delivery_idx].delivery_id = 0;           /* delivery: DELIVERY=0 */
    }

    /* Write output file */
    snprintf(path, sizeof(path), "%s/%s.txt", output_dir, name);
    fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "Error: cannot open %s for writing\n", path);
        free(px); free(py); free(nodes);
        return;
    }

    /* Header line: K Q S (vehicles, capacity, speed) */
    fprintf(fp, "%d\t%d\t1\n", num_vehicles, capacity);

    /* All nodes: ID  X  Y  DEMAND  TW_EARLY  TW_LATE  SERVICE  PICKUP  DELIVERY */
    for (i = 0; i < total_nodes; i++) {
        fprintf(fp, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
                nodes[i].id,
                nodes[i].x,
                nodes[i].y,
                nodes[i].demand,
                nodes[i].tw_early,
                nodes[i].tw_late,
                nodes[i].service_time,
                nodes[i].pickup_id,
                nodes[i].delivery_id);
    }

    fclose(fp);
    free(px);
    free(py);
    free(nodes);
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
    printf("Generate Li & Lim-format PDPTW benchmark instances.\n");
    printf("\n");
    printf("Options:\n");
    printf("  --size <n>        Number of requests (default: 100, generates 2n+1 nodes)\n");
    printf("  --class <name>    C1, C2, R1, R2, RC1, RC2, or ALL (default: ALL)\n");
    printf("  --count <n>       Instances per class (default: 5)\n");
    printf("  --seed <n>        Base seed (default: 42)\n");
    printf("  --output-dir <p>  Output directory (default: benchmarks/generated/pdptw)\n");
    printf("  --help            Show this help\n");
}

/*
 * Class name matching: user specifies Solomon-style names (C1, R2, RC1, etc.)
 * and we map them to Li-Lim prefixes (LC1, LR2, LRC1, etc.)
 */
static int find_class_index(const char *name) {
    int i;
    /* Try exact match with Li-Lim prefix first */
    for (i = 0; i < NUM_CLASSES; i++) {
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
    /* Try without 'L' prefix: "C1" -> "LC1", "RC2" -> "LRC2" */
    for (i = 0; i < NUM_CLASSES; i++) {
        const char *a = ALL_CLASSES[i].name + 1; /* skip leading 'L' */
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
    int n_requests = 100;
    int count = 5;
    uint64_t base_seed = 42;
    const char *output_dir = "benchmarks/generated/pdptw";
    int class_filter = -1;
    int total_generated = 0;
    int i, ci;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            n_requests = atoi(argv[++i]);
            if (n_requests < 1 || n_requests > 10000) {
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
                    fprintf(stderr, "  (also accepts LC1, LC2, LR1, LR2, LRC1, LRC2)\n");
                    return 1;
                }
            }
            continue;
        }
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
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

    printf("sg_gen_li_lim: requests=%d count=%d seed=%llu output=%s\n",
           n_requests, count, (unsigned long long)base_seed, output_dir);

    for (ci = 0; ci < NUM_CLASSES; ci++) {
        int inst;

        if (class_filter >= 0 && ci != class_filter) continue;

        mkdirs(output_dir);

        for (inst = 0; inst < count; inst++) {
            char name[64];
            uint64_t seed = instance_seed(base_seed, ci, n_requests, inst);

            snprintf(name, sizeof(name), "%s_%03d_%02d",
                     ALL_CLASSES[ci].name, n_requests, inst + 1);

            printf("  Generating %s (%d requests, %d nodes) ...\n",
                   name, n_requests, 2 * n_requests + 1);
            generate_instance(&ALL_CLASSES[ci], n_requests, seed,
                              name, output_dir);
            total_generated++;
        }
    }

    printf("Done: %d instances generated in %s/\n", total_generated, output_dir);
    return 0;
}
