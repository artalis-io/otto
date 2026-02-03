/*
 * bench_netflow.c - Benchmarks for Network Simplex Solver
 *
 * Compares network simplex performance on various problem types and sizes.
 * Also benchmarks warm start, cost scaling, flow decomposition, and bottleneck.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "netflow.h"
#include "ralph.h"

/* ============================================================================
 * Timing utilities
 * ============================================================================ */

typedef struct {
    clock_t start;
    double elapsed_ms;
} Timer;

static void timer_start(Timer *t) {
    t->start = clock();
}

static void timer_stop(Timer *t) {
    t->elapsed_ms = (double)(clock() - t->start) / CLOCKS_PER_SEC * 1000.0;
}

/* ============================================================================
 * Problem generators
 * ============================================================================ */

/*
 * Generate a transportation problem (bipartite network).
 * Sources: nodes 0 to num_sources-1
 * Sinks: nodes num_sources to num_sources+num_sinks-1
 */
typedef struct {
    int num_nodes;
    int num_arcs;
    int *tail;
    int *head;
    double *cost;
    double *capacity;
    double *supply;
} NetworkProblem;

static void free_problem(NetworkProblem *p) {
    free(p->tail);
    free(p->head);
    free(p->cost);
    free(p->capacity);
    free(p->supply);
}

static NetworkProblem generate_transportation(int num_sources, int num_sinks, int seed) {
    srand(seed);

    int num_nodes = num_sources + num_sinks;
    int num_arcs = num_sources * num_sinks;

    NetworkProblem p;
    p.num_nodes = num_nodes;
    p.num_arcs = num_arcs;
    p.tail = malloc(num_arcs * sizeof(int));
    p.head = malloc(num_arcs * sizeof(int));
    p.cost = malloc(num_arcs * sizeof(double));
    p.capacity = malloc(num_arcs * sizeof(double));
    p.supply = calloc(num_nodes, sizeof(double));

    /* Create arcs from each source to each sink */
    int arc = 0;
    for (int i = 0; i < num_sources; i++) {
        for (int j = 0; j < num_sinks; j++) {
            p.tail[arc] = i;
            p.head[arc] = num_sources + j;
            p.cost[arc] = (rand() % 10000) / 100.0;  /* 0 to 99.99 */
            p.capacity[arc] = RALPH_NETFLOW_INFINITY;
            arc++;
        }
    }

    /* Balanced supply/demand */
    double total_supply = (rand() % 1000 + 100) * num_sources;
    for (int i = 0; i < num_sources; i++) {
        p.supply[i] = total_supply / num_sources + (rand() % 100 - 50);
    }

    /* Adjust last source to ensure exact balance */
    double actual_supply = 0;
    for (int i = 0; i < num_sources; i++) {
        actual_supply += p.supply[i];
    }

    for (int j = 0; j < num_sinks; j++) {
        p.supply[num_sources + j] = -actual_supply / num_sinks;
    }

    /* Adjust last sink for exact balance */
    double check = 0;
    for (int i = 0; i < num_nodes - 1; i++) {
        check += p.supply[i];
    }
    p.supply[num_nodes - 1] = -check;

    return p;
}

/*
 * Generate a sparse network (not bipartite).
 */
static NetworkProblem generate_sparse_network(int num_nodes, double density, int seed) {
    srand(seed);

    /* Count arcs */
    int max_arcs = num_nodes * (num_nodes - 1);
    int target_arcs = (int)(max_arcs * density);
    if (target_arcs < num_nodes) target_arcs = num_nodes;  /* At least a spanning tree */

    int *tail = malloc(max_arcs * sizeof(int));
    int *head = malloc(max_arcs * sizeof(int));
    double *cost = malloc(max_arcs * sizeof(double));

    int num_arcs = 0;

    /* First create a chain to ensure connectivity */
    for (int i = 0; i < num_nodes - 1; i++) {
        tail[num_arcs] = i;
        head[num_arcs] = i + 1;
        cost[num_arcs] = (rand() % 10000) / 100.0;
        num_arcs++;
    }

    /* Add random arcs up to target */
    while (num_arcs < target_arcs) {
        int i = rand() % num_nodes;
        int j = rand() % num_nodes;
        if (i != j) {
            tail[num_arcs] = i;
            head[num_arcs] = j;
            cost[num_arcs] = (rand() % 10000) / 100.0;
            num_arcs++;
        }
    }

    NetworkProblem p;
    p.num_nodes = num_nodes;
    p.num_arcs = num_arcs;
    p.tail = realloc(tail, num_arcs * sizeof(int));
    p.head = realloc(head, num_arcs * sizeof(int));
    p.cost = realloc(cost, num_arcs * sizeof(double));
    p.capacity = malloc(num_arcs * sizeof(double));
    p.supply = calloc(num_nodes, sizeof(double));

    for (int a = 0; a < num_arcs; a++) {
        p.capacity[a] = RALPH_NETFLOW_INFINITY;
    }

    /* Source at node 0, sink at last node */
    double flow_amount = rand() % 1000 + 100;
    p.supply[0] = flow_amount;
    p.supply[num_nodes - 1] = -flow_amount;

    return p;
}

/*
 * Generate a grid network (for shortest path style problems).
 */
static NetworkProblem generate_grid(int width, int height, int seed) {
    srand(seed);

    int num_nodes = width * height;
    /* Horizontal + vertical arcs (both directions) */
    int num_arcs = 2 * ((width - 1) * height + width * (height - 1));

    NetworkProblem p;
    p.num_nodes = num_nodes;
    p.num_arcs = num_arcs;
    p.tail = malloc(num_arcs * sizeof(int));
    p.head = malloc(num_arcs * sizeof(int));
    p.cost = malloc(num_arcs * sizeof(double));
    p.capacity = malloc(num_arcs * sizeof(double));
    p.supply = calloc(num_nodes, sizeof(double));

    int arc = 0;

    /* Horizontal arcs */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width - 1; x++) {
            int node = y * width + x;
            /* Forward */
            p.tail[arc] = node;
            p.head[arc] = node + 1;
            p.cost[arc] = (rand() % 100) / 10.0 + 1.0;
            p.capacity[arc] = RALPH_NETFLOW_INFINITY;
            arc++;
            /* Backward */
            p.tail[arc] = node + 1;
            p.head[arc] = node;
            p.cost[arc] = (rand() % 100) / 10.0 + 1.0;
            p.capacity[arc] = RALPH_NETFLOW_INFINITY;
            arc++;
        }
    }

    /* Vertical arcs */
    for (int y = 0; y < height - 1; y++) {
        for (int x = 0; x < width; x++) {
            int node = y * width + x;
            /* Forward */
            p.tail[arc] = node;
            p.head[arc] = node + width;
            p.cost[arc] = (rand() % 100) / 10.0 + 1.0;
            p.capacity[arc] = RALPH_NETFLOW_INFINITY;
            arc++;
            /* Backward */
            p.tail[arc] = node + width;
            p.head[arc] = node;
            p.cost[arc] = (rand() % 100) / 10.0 + 1.0;
            p.capacity[arc] = RALPH_NETFLOW_INFINITY;
            arc++;
        }
    }

    /* Source at top-left, sink at bottom-right */
    double flow_amount = rand() % 100 + 10;
    p.supply[0] = flow_amount;
    p.supply[num_nodes - 1] = -flow_amount;

    return p;
}

/* ============================================================================
 * Benchmark: Size scaling (transportation)
 * ============================================================================ */

static void bench_size_scaling(void) {
    printf("\n");
    printf("======================================================================\n");
    printf("  Size Scaling Benchmark (Transportation Problems)\n");
    printf("======================================================================\n");
    printf("  %-10s %8s %8s %12s %10s %10s\n",
           "Problem", "Nodes", "Arcs", "Time (ms)", "Iters", "Objective");
    printf("  --------------------------------------------------------------------\n");

    int sizes[][2] = {
        {5, 5},      /* 10 nodes, 25 arcs */
        {10, 10},    /* 20 nodes, 100 arcs */
        {20, 20},    /* 40 nodes, 400 arcs */
        {50, 50},    /* 100 nodes, 2500 arcs */
        {100, 100},  /* 200 nodes, 10000 arcs */
        {200, 200},  /* 400 nodes, 40000 arcs */
        {500, 500},  /* 1000 nodes, 250000 arcs */
    };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int ns = sizes[s][0];
        int nd = sizes[s][1];

        NetworkProblem np = generate_transportation(ns, nd, 42 + s);

        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes,
            .num_arcs = np.num_arcs,
            .tail = np.tail,
            .head = np.head,
            .cost = np.cost,
            .capacity = np.capacity,
            .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };

        double *flow = malloc(np.num_arcs * sizeof(double));
        RalphNetflowResult result = {.flow = flow};
        RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;

        int trials = (np.num_arcs <= 10000) ? 10 : (np.num_arcs <= 100000) ? 5 : 3;

        Timer timer;
        double total_time = 0;

        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve(&prob, &opts, &result, NULL);
            timer_stop(&timer);
            total_time += timer.elapsed_ms;
        }

        char name[32];
        snprintf(name, sizeof(name), "%dx%d", ns, nd);

        printf("  %-10s %8d %8d %12.3f %10lld %10.2f\n",
               name, np.num_nodes, np.num_arcs,
               total_time / trials, (long long)result.iterations, result.objective);

        free(flow);
        free_problem(&np);
    }
}

/* ============================================================================
 * Benchmark: Problem types
 * ============================================================================ */

static void bench_problem_types(void) {
    printf("\n");
    printf("======================================================================\n");
    printf("  Problem Type Comparison\n");
    printf("======================================================================\n");
    printf("  %-20s %8s %8s %12s %10s\n",
           "Type", "Nodes", "Arcs", "Time (ms)", "Objective");
    printf("  --------------------------------------------------------------------\n");

    Timer timer;
    int trials = 10;

    /* Transportation 20x20 */
    {
        NetworkProblem np = generate_transportation(20, 20, 123);
        double *flow = malloc(np.num_arcs * sizeof(double));

        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes, .num_arcs = np.num_arcs,
            .tail = np.tail, .head = np.head, .cost = np.cost,
            .capacity = np.capacity, .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};

        double total = 0;
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve(&prob, NULL, &result, NULL);
            timer_stop(&timer);
            total += timer.elapsed_ms;
        }

        printf("  %-20s %8d %8d %12.3f %10.2f\n",
               "Transportation", np.num_nodes, np.num_arcs, total / trials, result.objective);

        free(flow);
        free_problem(&np);
    }

    /* Sparse network */
    {
        NetworkProblem np = generate_sparse_network(100, 0.1, 123);
        double *flow = malloc(np.num_arcs * sizeof(double));

        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes, .num_arcs = np.num_arcs,
            .tail = np.tail, .head = np.head, .cost = np.cost,
            .capacity = np.capacity, .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};

        double total = 0;
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve(&prob, NULL, &result, NULL);
            timer_stop(&timer);
            total += timer.elapsed_ms;
        }

        printf("  %-20s %8d %8d %12.3f %10.2f\n",
               "Sparse (10%)", np.num_nodes, np.num_arcs, total / trials, result.objective);

        free(flow);
        free_problem(&np);
    }

    /* Grid network */
    {
        NetworkProblem np = generate_grid(20, 20, 123);
        double *flow = malloc(np.num_arcs * sizeof(double));

        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes, .num_arcs = np.num_arcs,
            .tail = np.tail, .head = np.head, .cost = np.cost,
            .capacity = np.capacity, .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};

        double total = 0;
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve(&prob, NULL, &result, NULL);
            timer_stop(&timer);
            total += timer.elapsed_ms;
        }

        printf("  %-20s %8d %8d %12.3f %10.2f\n",
               "Grid 20x20", np.num_nodes, np.num_arcs, total / trials, result.objective);

        free(flow);
        free_problem(&np);
    }
}

/* ============================================================================
 * Benchmark: Warm start
 * ============================================================================ */

static void bench_warm_start(void) {
    printf("\n");
    printf("======================================================================\n");
    printf("  Warm Start Benchmark\n");
    printf("======================================================================\n");
    printf("  %-10s %10s %10s %10s %10s\n",
           "Problem", "Cold (ms)", "Warm (ms)", "Speedup", "Status");
    printf("  --------------------------------------------------------------------\n");

    int sizes[][2] = {{10, 10}, {20, 20}, {50, 50}, {100, 100}};
    int num_sizes = 4;
    int num_resolves = 10;

    for (int s = 0; s < num_sizes; s++) {
        int ns = sizes[s][0];
        int nd = sizes[s][1];

        NetworkProblem np = generate_transportation(ns, nd, 42);

        double *flow = malloc(np.num_arcs * sizeof(double));
        RalphNetflowWorkspace *ws = ralph_netflow_workspace_create(np.num_nodes, np.num_arcs);

        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes, .num_arcs = np.num_arcs,
            .tail = np.tail, .head = np.head, .cost = np.cost,
            .capacity = np.capacity, .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};
        RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;
        opts.save_warm_start = 1;

        Timer timer;
        double cold_total = 0, warm_total = 0;
        double cold_obj = 0, warm_obj = 0;

        /* Cold start benchmark */
        for (int r = 0; r < num_resolves; r++) {
            /* Perturb costs slightly */
            for (int a = 0; a < np.num_arcs; a++) {
                np.cost[a] += (rand() % 100 - 50) / 100.0;
            }

            opts.warm_start = 0;
            timer_start(&timer);
            ralph_netflow_solve(&prob, &opts, &result, NULL);
            timer_stop(&timer);
            cold_total += timer.elapsed_ms;
            cold_obj = result.objective;
        }

        /* Reset costs */
        srand(42);
        for (int a = 0; a < np.num_arcs; a++) {
            np.cost[a] = (rand() % 10000) / 100.0;
        }
        ralph_netflow_warm_start_clear(ws);

        /* Warm start benchmark */
        for (int r = 0; r < num_resolves; r++) {
            for (int a = 0; a < np.num_arcs; a++) {
                np.cost[a] += (rand() % 100 - 50) / 100.0;
            }

            opts.warm_start = 1;
            timer_start(&timer);
            ralph_netflow_solve(&prob, &opts, &result, ws);
            timer_stop(&timer);
            warm_total += timer.elapsed_ms;
            warm_obj = result.objective;
        }

        double speedup = cold_total / warm_total;
        const char *status = (fabs(cold_obj - warm_obj) < 1.0) ? "OK" : "DIFF";

        char name[32];
        snprintf(name, sizeof(name), "%dx%d", ns, nd);

        printf("  %-10s %10.3f %10.3f %10.2fx %10s\n",
               name, cold_total / num_resolves, warm_total / num_resolves, speedup, status);

        free(flow);
        ralph_netflow_workspace_free(ws);
        free_problem(&np);
    }
}

/* ============================================================================
 * Benchmark: Cost scaling
 * ============================================================================ */

static void bench_cost_scaling(void) {
    printf("\n");
    printf("======================================================================\n");
    printf("  Cost Scaling Benchmark (Degenerate Problems)\n");
    printf("======================================================================\n");
    printf("  %-10s %12s %12s %10s %10s\n",
           "Problem", "No Scale", "Scaling", "Speedup", "Status");
    printf("  --------------------------------------------------------------------\n");

    /* Create degenerate problems with many equal costs */
    int sizes[][2] = {{10, 10}, {20, 20}, {50, 50}};
    int num_sizes = 3;
    int trials = 5;

    for (int s = 0; s < num_sizes; s++) {
        int ns = sizes[s][0];
        int nd = sizes[s][1];
        int num_nodes = ns + nd;
        int num_arcs = ns * nd;

        int *tail = malloc(num_arcs * sizeof(int));
        int *head = malloc(num_arcs * sizeof(int));
        double *cost = malloc(num_arcs * sizeof(double));
        double *supply = calloc(num_nodes, sizeof(double));
        double *flow = malloc(num_arcs * sizeof(double));

        /* Create degenerate transportation (all costs equal) */
        int arc = 0;
        for (int i = 0; i < ns; i++) {
            for (int j = 0; j < nd; j++) {
                tail[arc] = i;
                head[arc] = ns + j;
                cost[arc] = 10.0;  /* All equal - highly degenerate */
                arc++;
            }
        }

        /* Balanced supply/demand */
        for (int i = 0; i < ns; i++) supply[i] = 10.0;
        for (int j = 0; j < nd; j++) supply[ns + j] = -10.0 * ns / nd;

        /* Adjust for exact balance */
        double total = 0;
        for (int i = 0; i < num_nodes; i++) total += supply[i];
        supply[num_nodes - 1] -= total;

        RalphNetflowProblem prob = {
            .num_nodes = num_nodes, .num_arcs = num_arcs,
            .tail = tail, .head = head, .cost = cost,
            .capacity = NULL, .supply = supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};
        RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;

        Timer timer;
        double no_scale_total = 0, scale_total = 0;
        double no_scale_obj = 0, scale_obj = 0;

        /* Without cost scaling */
        opts.cost_scaling = 0;
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve(&prob, &opts, &result, NULL);
            timer_stop(&timer);
            no_scale_total += timer.elapsed_ms;
            no_scale_obj = result.objective;
        }

        /* With cost scaling */
        opts.cost_scaling = 1;
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve(&prob, &opts, &result, NULL);
            timer_stop(&timer);
            scale_total += timer.elapsed_ms;
            scale_obj = result.objective;
        }

        double speedup = no_scale_total / scale_total;
        const char *status = (fabs(no_scale_obj - scale_obj) < 0.01) ? "OK" : "DIFF";

        char name[32];
        snprintf(name, sizeof(name), "%dx%d", ns, nd);

        printf("  %-10s %12.3f %12.3f %10.2fx %10s\n",
               name, no_scale_total / trials, scale_total / trials, speedup, status);

        free(tail);
        free(head);
        free(cost);
        free(supply);
        free(flow);
    }
}

/* ============================================================================
 * Benchmark: Flow decomposition
 * ============================================================================ */

static void bench_flow_decomposition(void) {
    printf("\n");
    printf("======================================================================\n");
    printf("  Flow Decomposition Benchmark\n");
    printf("======================================================================\n");
    printf("  %-10s %8s %10s %10s %10s\n",
           "Problem", "Paths", "Solve (ms)", "Decomp (ms)", "Total (ms)");
    printf("  --------------------------------------------------------------------\n");

    int sizes[][2] = {{10, 10}, {20, 20}, {50, 50}};
    int num_sizes = 3;
    int trials = 10;

    for (int s = 0; s < num_sizes; s++) {
        int ns = sizes[s][0];
        int nd = sizes[s][1];

        NetworkProblem np = generate_transportation(ns, nd, 42);

        double *flow = malloc(np.num_arcs * sizeof(double));
        int max_paths = np.num_arcs;
        RalphNetflowPath *paths = malloc(max_paths * sizeof(RalphNetflowPath));

        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes, .num_arcs = np.num_arcs,
            .tail = np.tail, .head = np.head, .cost = np.cost,
            .capacity = np.capacity, .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};

        Timer timer;
        double solve_total = 0, decomp_total = 0;
        int num_paths = 0;

        for (int t = 0; t < trials; t++) {
            /* Solve */
            timer_start(&timer);
            ralph_netflow_solve(&prob, NULL, &result, NULL);
            timer_stop(&timer);
            solve_total += timer.elapsed_ms;

            /* Decompose */
            timer_start(&timer);
            ralph_netflow_decompose(&prob, flow, max_paths, paths, &num_paths);
            timer_stop(&timer);
            decomp_total += timer.elapsed_ms;

            /* Free path arcs */
            for (int i = 0; i < num_paths; i++) {
                ralph_netflow_path_free(&paths[i]);
            }
        }

        char name[32];
        snprintf(name, sizeof(name), "%dx%d", ns, nd);

        printf("  %-10s %8d %10.3f %10.3f %10.3f\n",
               name, num_paths, solve_total / trials, decomp_total / trials,
               (solve_total + decomp_total) / trials);

        free(flow);
        free(paths);
        free_problem(&np);
    }
}

/* ============================================================================
 * Benchmark: Bottleneck network flow
 * ============================================================================ */

static void bench_bottleneck(void) {
    printf("\n");
    printf("======================================================================\n");
    printf("  Bottleneck Network Flow Benchmark\n");
    printf("======================================================================\n");
    printf("  %-10s %10s %10s %12s %12s\n",
           "Problem", "MCNF (ms)", "Bottleneck", "MCNF Obj", "Bottleneck");
    printf("  --------------------------------------------------------------------\n");

    int sizes[][2] = {{10, 10}, {20, 20}, {50, 50}};
    int num_sizes = 3;
    int trials = 10;

    for (int s = 0; s < num_sizes; s++) {
        int ns = sizes[s][0];
        int nd = sizes[s][1];

        NetworkProblem np = generate_transportation(ns, nd, 42);

        double *flow = malloc(np.num_arcs * sizeof(double));

        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes, .num_arcs = np.num_arcs,
            .tail = np.tail, .head = np.head, .cost = np.cost,
            .capacity = np.capacity, .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};
        RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;

        Timer timer;
        double mcnf_total = 0, bottleneck_total = 0;
        double mcnf_obj = 0, bottleneck_obj = 0;

        /* Standard MCNF */
        opts.algorithm = RALPH_NETFLOW_ALG_STANDARD;
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve_ex(&prob, &opts, &result, NULL);
            timer_stop(&timer);
            mcnf_total += timer.elapsed_ms;
            mcnf_obj = result.objective;
        }

        /* Bottleneck */
        opts.algorithm = RALPH_NETFLOW_ALG_BOTTLENECK;
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve_ex(&prob, &opts, &result, NULL);
            timer_stop(&timer);
            bottleneck_total += timer.elapsed_ms;
            bottleneck_obj = result.objective;
        }

        char name[32];
        snprintf(name, sizeof(name), "%dx%d", ns, nd);

        printf("  %-10s %10.3f %10.3f %12.2f %12.2f\n",
               name, mcnf_total / trials, bottleneck_total / trials,
               mcnf_obj, bottleneck_obj);

        free(flow);
        free_problem(&np);
    }
}

/* ============================================================================
 * Benchmark: Network simplex vs LP
 * ============================================================================ */

static void bench_vs_lp(void) {
    printf("\n");
    printf("======================================================================\n");
    printf("  Network Simplex vs LP Simplex\n");
    printf("======================================================================\n");
    printf("  %-10s %10s %10s %10s %10s\n",
           "Problem", "NetSim (ms)", "LP (ms)", "Speedup", "Status");
    printf("  --------------------------------------------------------------------\n");

    /* Only small problems for LP (it's much slower) */
    int sizes[][2] = {{5, 5}, {10, 10}, {15, 15}, {20, 20}};
    int num_sizes = 4;
    int trials = 5;

    for (int s = 0; s < num_sizes; s++) {
        int ns = sizes[s][0];
        int nd = sizes[s][1];

        NetworkProblem np = generate_transportation(ns, nd, 42);

        double *flow = malloc(np.num_arcs * sizeof(double));

        /* Network simplex */
        RalphNetflowProblem prob = {
            .num_nodes = np.num_nodes, .num_arcs = np.num_arcs,
            .tail = np.tail, .head = np.head, .cost = np.cost,
            .capacity = np.capacity, .supply = np.supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };
        RalphNetflowResult result = {.flow = flow};

        Timer timer;
        double netsim_total = 0;
        double netsim_obj = 0;

        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_netflow_solve(&prob, NULL, &result, NULL);
            timer_stop(&timer);
            netsim_total += timer.elapsed_ms;
            netsim_obj = result.objective;
        }

        /* LP formulation */
        double lp_total = 0;
        double lp_obj = 0;

        for (int t = 0; t < trials; t++) {
            RalphModel *model = ralph_create();
            ralph_set_obj_sense(model, RALPH_MINIMIZE);

            /* Add arc flow variables */
            for (int a = 0; a < np.num_arcs; a++) {
                double cap = np.capacity ? np.capacity[a] : 1e15;
                ralph_add_var(model, 0.0, cap, np.cost[a], RALPH_CONTINUOUS);
            }

            /* Flow conservation constraints */
            int *idx = malloc(np.num_arcs * sizeof(int));
            double *val = malloc(np.num_arcs * sizeof(double));

            for (int i = 0; i < np.num_nodes; i++) {
                int len = 0;
                for (int a = 0; a < np.num_arcs; a++) {
                    if (np.tail[a] == i) {
                        idx[len] = a;
                        val[len] = 1.0;
                        len++;
                    } else if (np.head[a] == i) {
                        idx[len] = a;
                        val[len] = -1.0;
                        len++;
                    }
                }
                ralph_add_constraint(model, len, idx, val, RALPH_EQUAL, np.supply[i]);
            }

            free(idx);
            free(val);

            timer_start(&timer);
            ralph_optimize(model);
            timer_stop(&timer);
            lp_total += timer.elapsed_ms;
            lp_obj = ralph_get_objval(model);

            ralph_free(model);
        }

        double speedup = lp_total / netsim_total;
        const char *status = (fabs(netsim_obj - lp_obj) < 1.0) ? "OK" : "DIFF";

        char name[32];
        snprintf(name, sizeof(name), "%dx%d", ns, nd);

        printf("  %-10s %10.3f %10.3f %10.1fx %10s\n",
               name, netsim_total / trials, lp_total / trials, speedup, status);

        free(flow);
        free_problem(&np);
    }

    printf("\n  Note: Network simplex exploits network structure for faster solving.\n");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    printf("\n");
    printf("======================================================================\n");
    printf("                                                                      \n");
    printf("   Ralph Network Simplex Solver Benchmarks                            \n");
    printf("   Minimum Cost Network Flow (MCNF)                                   \n");
    printf("                                                                      \n");
    printf("======================================================================\n");

    int run_all = (argc < 2);
    int run_size = run_all || (argc > 1 && strcmp(argv[1], "size") == 0);
    int run_types = run_all || (argc > 1 && strcmp(argv[1], "types") == 0);
    int run_warm = run_all || (argc > 1 && strcmp(argv[1], "warm") == 0);
    int run_scaling = run_all || (argc > 1 && strcmp(argv[1], "scaling") == 0);
    int run_decomp = run_all || (argc > 1 && strcmp(argv[1], "decomp") == 0);
    int run_bottleneck = run_all || (argc > 1 && strcmp(argv[1], "bottleneck") == 0);
    int run_lp = run_all || (argc > 1 && strcmp(argv[1], "lp") == 0);

    if (run_size) bench_size_scaling();
    if (run_types) bench_problem_types();
    if (run_warm) bench_warm_start();
    if (run_scaling) bench_cost_scaling();
    if (run_decomp) bench_flow_decomposition();
    if (run_bottleneck) bench_bottleneck();
    if (run_lp) bench_vs_lp();

    printf("\n");
    return 0;
}
