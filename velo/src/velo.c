/*
 * velo.c - Main API implementation
 *
 * High-level API for loading graphs and computing routes.
 */

#include "velo.h"
#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * Version
 * ============================================================================ */

const char *vl_version(void)
{
    return VL_VERSION_STRING;
}

/* ============================================================================
 * Status Messages
 * ============================================================================ */

const char *vl_status_string(VLStatus status)
{
    switch (status) {
    case VL_OK:                      return "Success";
    case VL_ERROR_INVALID_ARGUMENT:  return "Invalid argument";
    case VL_ERROR_OUT_OF_MEMORY:     return "Out of memory";
    case VL_ERROR_FILE_NOT_FOUND:    return "File not found";
    case VL_ERROR_FILE_READ:         return "File read error";
    case VL_ERROR_FILE_WRITE:        return "File write error";
    case VL_ERROR_PARSE_ERROR:       return "Parse error";
    case VL_ERROR_NO_ROUTE:          return "No route found";
    case VL_ERROR_NODE_NOT_FOUND:    return "Node not found";
    case VL_ERROR_GRAPH_NOT_LOADED:  return "Graph not loaded";
    case VL_ERROR_UNSUPPORTED_FORMAT: return "Unsupported format";
    case VL_ERROR_INTERNAL:          return "Internal error";
    default:                         return "Unknown error";
    }
}

/* ============================================================================
 * High-Level Loading
 * ============================================================================ */

VLGraph *vl_load_pbf(const char *filename)
{
    if (!filename) return NULL;

    /* Create PBF context */
    VLPBFContext *ctx = vl_pbf_context_create();
    if (!ctx) return NULL;

    /* Parse PBF file */
    VLStatus status = vl_pbf_parse_file(ctx, filename);
    if (status != VL_OK) {
        fprintf(stderr, "velo: Failed to parse PBF: %s\n", vl_status_string(status));
        vl_pbf_context_free(ctx);
        return NULL;
    }

    fprintf(stderr, "velo: Parsed %zu nodes, %zu ways (%zu highway ways)\n",
            ctx->total_nodes_parsed, ctx->total_ways_parsed, ctx->highway_ways_kept);

    /* Create graph builder */
    VLGraphBuilder *builder = vl_graph_builder_create(ctx->num_nodes);
    if (!builder) {
        vl_pbf_context_free(ctx);
        return NULL;
    }

    /* Build graph from PBF data */
    status = vl_graph_build_from_pbf(builder, ctx);
    if (status != VL_OK) {
        fprintf(stderr, "velo: Failed to build graph: %s\n", vl_status_string(status));
        vl_graph_builder_free(builder);
        vl_pbf_context_free(ctx);
        return NULL;
    }

    /* Done with PBF context */
    vl_pbf_context_free(ctx);

    /* Finalize graph */
    VLGraph *graph = vl_graph_finalize(builder);
    vl_graph_builder_free(builder);

    if (graph) {
        uint32_t nodes, edges;
        double avg_degree;
        vl_graph_stats(graph, &nodes, &edges, NULL, &avg_degree);
        fprintf(stderr, "velo: Graph built: %u nodes, %u edges (avg degree: %.2f)\n",
                nodes, edges, avg_degree);
    }

    return graph;
}

VLGraph *vl_load_binary(const char *filename)
{
    if (!filename) return NULL;

#ifndef _WIN32
    /* Try mmap first for better performance */
    VLGraph *graph = vl_graph_mmap(filename);
    if (graph) return graph;
#endif

    /* Fall back to regular load */
    return vl_graph_load(filename);
}

VLStatus vl_save_binary(const VLGraph *graph, const char *filename)
{
    return vl_graph_save(graph, filename);
}

/* ============================================================================
 * Coordinate Validation
 * ============================================================================ */

/* Already implemented in vl_geo.c, just provide the declaration */
/* int vl_coord_valid(VLCoord c) is defined in vl_geo.c */
