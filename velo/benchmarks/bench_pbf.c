/*
 * bench_pbf.c - PBF parsing benchmarks
 *
 * Measures PBF parsing and graph construction performance.
 */

#include "velo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double get_time_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <sys/time.h>
#include <sys/stat.h>
static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
#endif

static size_t get_file_size(const char *filename)
{
#ifndef _WIN32
    struct stat st;
    if (stat(filename, &st) == 0) {
        return (size_t)st.st_size;
    }
#endif
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size > 0 ? (size_t)size : 0;
}

/* Forward declaration */
VLStatus vl_graph_contract_degree2(VLGraph *graph);

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <file.osm.pbf> [output.vlg] [--contract]\n", argv[0]);
        printf("  --contract: Apply degree-2 node contraction\n");
        return 1;
    }

    const char *pbf_file = argv[1];
    const char *vlg_file = NULL;
    int do_contract = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--contract") == 0) {
            do_contract = 1;
        } else if (argv[i][0] != '-') {
            vlg_file = argv[i];
        }
    }

    printf("Velo PBF Parsing Benchmark\n");
    printf("==========================\n\n");

    size_t file_size = get_file_size(pbf_file);
    printf("Input file: %s (%.2f MB)\n\n", pbf_file, file_size / (1024.0 * 1024.0));

    /* Phase 1: Parse PBF */
    printf("Phase 1: Parsing PBF...\n");
    double t1 = get_time_ms();

    VLPBFContext *ctx = vl_pbf_context_create();
    if (!ctx) {
        printf("Failed to create PBF context\n");
        return 1;
    }

    VLStatus status = vl_pbf_parse_file(ctx, pbf_file);
    double t2 = get_time_ms();

    if (status != VL_OK) {
        printf("Failed to parse PBF: %s\n", vl_status_string(status));
        vl_pbf_context_free(ctx);
        return 1;
    }

    double parse_time = t2 - t1;
    double parse_speed = (file_size / (1024.0 * 1024.0)) / (parse_time / 1000.0);

    printf("  Nodes parsed:    %zu\n", ctx->total_nodes_parsed);
    printf("  Ways parsed:     %zu\n", ctx->total_ways_parsed);
    printf("  Highway ways:    %zu\n", ctx->highway_ways_kept);
    printf("  Nodes retained:  %zu\n", ctx->num_nodes);
    printf("  Parse time:      %.2f ms (%.2f MB/s)\n\n", parse_time, parse_speed);

    /* Phase 2: Build graph */
    printf("Phase 2: Building graph...\n");
    t1 = get_time_ms();

    VLGraphBuilder *builder = vl_graph_builder_create(ctx->num_nodes);
    if (!builder) {
        printf("Failed to create graph builder\n");
        vl_pbf_context_free(ctx);
        return 1;
    }

    status = vl_graph_build_from_pbf(builder, ctx);
    t2 = get_time_ms();

    if (status != VL_OK) {
        printf("Failed to build graph: %s\n", vl_status_string(status));
        vl_graph_builder_free(builder);
        vl_pbf_context_free(ctx);
        return 1;
    }

    double build_time = t2 - t1;
    printf("  Build time:      %.2f ms\n\n", build_time);

    /* Done with PBF context */
    vl_pbf_context_free(ctx);

    /* Phase 3: Finalize graph */
    printf("Phase 3: Finalizing graph (CSR conversion)...\n");
    t1 = get_time_ms();

    VLGraph *graph = vl_graph_finalize(builder);
    t2 = get_time_ms();

    vl_graph_builder_free(builder);

    if (!graph) {
        printf("Failed to finalize graph\n");
        return 1;
    }

    double finalize_time = t2 - t1;

    uint32_t num_nodes, num_edges, max_degree;
    double avg_degree;
    vl_graph_stats(graph, &num_nodes, &num_edges, &max_degree, &avg_degree);

    printf("  Graph nodes:     %u\n", num_nodes);
    printf("  Graph edges:     %u\n", num_edges);
    printf("  Max out-degree:  %u\n", max_degree);
    printf("  Avg out-degree:  %.2f\n", avg_degree);
    printf("  Finalize time:   %.2f ms\n\n", finalize_time);

    /* Phase 3.5: Optional contraction */
    double contract_time = 0;
    if (do_contract) {
        printf("Phase 3.5: Contracting degree-2 nodes...\n");
        t1 = get_time_ms();

        status = vl_graph_contract_degree2(graph);
        t2 = get_time_ms();

        if (status == VL_OK) {
            contract_time = t2 - t1;
            vl_graph_stats(graph, &num_nodes, &num_edges, &max_degree, &avg_degree);
            printf("  Contracted nodes: %u\n", num_nodes);
            printf("  Contracted edges: %u\n", num_edges);
            printf("  Contract time:    %.2f ms\n\n", contract_time);
        } else {
            printf("  Contraction failed: %s\n\n", vl_status_string(status));
        }
    }

    /* Phase 4: Save to binary (optional) */
    if (vlg_file) {
        printf("Phase 4: Saving binary graph...\n");
        t1 = get_time_ms();

        status = vl_graph_save(graph, vlg_file);
        t2 = get_time_ms();

        if (status == VL_OK) {
            size_t vlg_size = get_file_size(vlg_file);
            double save_time = t2 - t1;
            printf("  Output file:     %s (%.2f MB)\n", vlg_file, vlg_size / (1024.0 * 1024.0));
            printf("  Save time:       %.2f ms\n\n", save_time);
        } else {
            printf("  Failed to save: %s\n\n", vl_status_string(status));
        }
    }

    /* Summary */
    double total_time = parse_time + build_time + finalize_time + contract_time;
    printf("Summary\n");
    printf("-------\n");
    printf("  Total time:      %.2f ms (%.2f seconds)\n", total_time, total_time / 1000.0);
    if (do_contract) {
        printf("  Contract time:   %.2f ms\n", contract_time);
    }
    printf("  Throughput:      %.2f MB/s\n", (file_size / (1024.0 * 1024.0)) / (total_time / 1000.0));

    /* Memory estimate */
    size_t node_mem = num_nodes * sizeof(VLNode);
    size_t edge_mem = num_edges * sizeof(VLEdge);
    size_t total_mem = node_mem + edge_mem + sizeof(VLGraph);
    printf("  Graph memory:    %.2f MB\n", total_mem / (1024.0 * 1024.0));

    /* Test binary load time */
    if (vlg_file) {
        printf("\nBinary Load Test\n");
        printf("----------------\n");

        t1 = get_time_ms();
        VLGraph *loaded = vl_graph_load(vlg_file);
        t2 = get_time_ms();

        if (loaded) {
            printf("  Load time:       %.2f ms\n", t2 - t1);
            vl_graph_free(loaded);
        }

#ifndef _WIN32
        t1 = get_time_ms();
        VLGraph *mmapped = vl_graph_mmap(vlg_file);
        t2 = get_time_ms();

        if (mmapped) {
            printf("  Mmap time:       %.2f ms\n", t2 - t1);
            /* Don't free mmapped graph - would need special handling */
            free(mmapped);  /* Only free the struct, not the data */
        }
#endif
    }

    vl_graph_free(graph);

    printf("\nDone.\n");
    return 0;
}
