/*
 * vl_types.h - Velo data structures
 *
 * Core data types for the Velo routing engine including coordinates,
 * graph storage (CSR format), route results, and configuration options.
 */

#ifndef VL_TYPES_H
#define VL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "sh_hashmap.h"
#include "sh_heap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    VL_OK = 0,
    VL_ERROR_INVALID_ARGUMENT,
    VL_ERROR_OUT_OF_MEMORY,
    VL_ERROR_FILE_NOT_FOUND,
    VL_ERROR_FILE_READ,
    VL_ERROR_FILE_WRITE,
    VL_ERROR_PARSE_ERROR,
    VL_ERROR_NO_ROUTE,
    VL_ERROR_NODE_NOT_FOUND,
    VL_ERROR_GRAPH_NOT_LOADED,
    VL_ERROR_UNSUPPORTED_FORMAT,
    VL_ERROR_INTERNAL
} VLStatus;

/* ============================================================================
 * Coordinates
 * ============================================================================ */

typedef struct {
    double lat;
    double lon;
} VLCoord;

/* Fixed-point coordinate (for compact storage) */
typedef struct {
    int32_t lat;  /* lat * 1e7 */
    int32_t lon;  /* lon * 1e7 */
} VLCoordFixed;

/* Convert between coordinate types */
#define VL_COORD_TO_FIXED(c) \
    ((VLCoordFixed){(int32_t)((c).lat * 1e7), (int32_t)((c).lon * 1e7)})
#define VL_FIXED_TO_COORD(f) \
    ((VLCoord){(f).lat * 1e-7, (f).lon * 1e-7})

/* ============================================================================
 * Graph Storage (CSR Format)
 * ============================================================================ */

/*
 * Edge in the graph.
 * Stored contiguously; node[i]'s outgoing edges are edges[edge_start..edge_start+edge_count).
 */
typedef struct {
    uint32_t target;      /* Target node index */
    uint32_t distance;    /* Distance in millimeters (max ~4295km per edge) */
    uint16_t duration;    /* Duration in deciseconds (max ~1.8 hours per edge) */
    uint16_t flags;       /* Edge flags (road type, one-way, etc.) */
} VLEdge;

/* Edge flags - bits 0-3: general flags, bits 4-7: road type, bits 8-15: access */
#define VL_EDGE_ONEWAY      0x0001
#define VL_EDGE_MOTORWAY    0x0010
#define VL_EDGE_TRUNK       0x0020
#define VL_EDGE_PRIMARY     0x0030
#define VL_EDGE_SECONDARY   0x0040
#define VL_EDGE_TERTIARY    0x0050
#define VL_EDGE_RESIDENTIAL 0x0060
#define VL_EDGE_SERVICE     0x0070
#define VL_EDGE_TYPE_MASK   0x00F0

/* Access restriction flags (from OSM access tags) */
#define VL_ACCESS_NO_CAR    0x0100  /* motor_vehicle=no or access=no */
#define VL_ACCESS_NO_TRUCK  0x0200  /* hgv=no */
#define VL_ACCESS_NO_BIKE   0x0400  /* bicycle=no */
#define VL_ACCESS_NO_FOOT   0x0800  /* foot=no */
#define VL_ACCESS_MASK      0x0F00

/*
 * Node in the graph.
 * Uses CSR (Compressed Sparse Row) format for edge storage.
 */
typedef struct {
    VLCoordFixed coord;    /* Node coordinates (fixed-point) */
    uint32_t edge_start;   /* Index of first outgoing edge */
    uint32_t edge_count;   /* Number of outgoing edges */
    int64_t osm_id;        /* Original OSM node ID */
} VLNode;

/*
 * Complete road network graph.
 * Uses CSR format: node[i]'s edges are edges[node[i].edge_start ... +edge_count).
 */
typedef struct {
    uint32_t num_nodes;
    VLNode *nodes;

    uint32_t num_edges;
    VLEdge *edges;

    /* Reverse graph (CSR format for incoming edges) */
    uint32_t *rev_edge_start;  /* Index of first incoming edge for each node */
    uint32_t *rev_edge_count;  /* Number of incoming edges for each node */
    uint32_t *rev_edges;       /* Source node index for each reverse edge */
    uint32_t *rev_edge_idx;    /* Original edge index (for weight lookup) */

    /* Grid spatial index for fast nearest-node queries */
    struct VLGridIndex *grid_index;

    /* Degree-2 contraction data (for path unpacking) */
    struct VLContraction *contraction;

    /* Bounding box */
    VLCoord bbox_min;
    VLCoord bbox_max;

    /* Memory management */
    int owns_memory;       /* 1 if we should free nodes/edges on destroy */
} VLGraph;

/*
 * Degree-2 contraction data.
 * When a chain A -> B -> C -> D is contracted to A -> D,
 * we store B, C for path reconstruction.
 */
typedef struct VLContraction {
    /* Mapping from contracted node to original node */
    uint32_t *node_to_original;     /* contracted_node -> original_node_id */
    uint32_t num_contracted_nodes;

    /* For each contracted edge, intermediate nodes (for path unpacking) */
    uint32_t *edge_intermediates;   /* Flattened array of intermediate nodes */
    uint32_t *edge_intermediate_offset;  /* Start offset for each edge */
    uint32_t num_intermediates;
} VLContraction;

/* ============================================================================
 * Grid Spatial Index
 * ============================================================================ */

#define VL_GRID_SIZE 1000  /* 1000x1000 grid cells */

typedef struct VLGridIndex {
    uint32_t *cell_nodes;      /* Node indices, grouped by cell */
    uint32_t *cell_offsets;    /* Start offset for each cell (size: GRID_SIZE^2 + 1) */
    double lat_min, lon_min;
    double cell_lat, cell_lon; /* Cell size in degrees */
} VLGridIndex;

/* ============================================================================
 * ALT (A* with Landmarks and Triangle inequality)
 * ============================================================================ */

#define VL_MAX_LANDMARKS 64
#define VL_DEFAULT_LANDMARKS 16  /* 32 gives better long-route performance */

typedef struct {
    int num_landmarks;
    uint32_t num_nodes;
    uint32_t *landmark_nodes;       /* Array of landmark node indices */

    /* Distance-based landmarks (for shortest path routing) */
    double *dist_to_landmark;       /* dist_to[k * num_nodes + v] = dist(v -> landmark k) */
    double *dist_from_landmark;     /* dist_from[k * num_nodes + v] = dist(landmark k -> v) */
    double *dist_to_t;              /* Transposed: dist_to_t[v * num_landmarks + k] */
    double *dist_from_t;            /* Transposed: dist_from_t[v * num_landmarks + k] */

    /* Duration-based landmarks (for fastest path routing) */
    double *time_to_landmark;       /* time_to[k * num_nodes + v] = time(v -> landmark k) */
    double *time_from_landmark;     /* time_from[k * num_nodes + v] = time(landmark k -> v) */
    double *time_to_t;              /* Transposed: time_to_t[v * num_landmarks + k] */
    double *time_from_t;            /* Transposed: time_from_t[v * num_landmarks + k] */
} VLLandmarks;

/* ============================================================================
 * Query Context (for lazy initialization and memory reuse)
 * ============================================================================ */

/* Use shared heap implementation */
typedef SHHeap VLHeap;

typedef struct {
    /* Distance arrays with timestamps for lazy init */
    double *dist_fwd;
    double *dist_bwd;
    uint32_t *parent_fwd;
    uint32_t *parent_bwd;
    uint32_t *timestamp_fwd;   /* Per-node timestamp for lazy init */
    uint32_t *timestamp_bwd;
    uint32_t current_timestamp;

    /* Heaps */
    VLHeap *heap_fwd;
    VLHeap *heap_bwd;

    /* Settled flags (reused with timestamp) */
    uint32_t *settled_timestamp_fwd;
    uint32_t *settled_timestamp_bwd;

    /* Graph reference */
    uint32_t num_nodes;
} VLQueryContext;

/* ============================================================================
 * Route Options
 * ============================================================================ */

typedef enum {
    VL_ALGORITHM_DIJKSTRA,
    VL_ALGORITHM_DIJKSTRA_BIDIR,
    VL_ALGORITHM_ASTAR,
    VL_ALGORITHM_ASTAR_BIDIR
} VLAlgorithm;

/* Vehicle profiles for routing */
typedef enum {
    VL_PROFILE_CAR = 0,    /* Standard car - all roads */
    VL_PROFILE_TRUCK,      /* HGV/truck - avoid residential, service */
    VL_PROFILE_BIKE,       /* Bicycle - avoid motorways, trunks */
    VL_PROFILE_FOOT,       /* Pedestrian - avoid motorways, trunks, primaries */
    VL_PROFILE_ANY         /* No filtering (all roads accessible) */
} VLProfile;

typedef enum {
    VL_WEIGHT_DISTANCE,    /* Optimize for shortest distance */
    VL_WEIGHT_DURATION     /* Optimize for fastest route */
} VLWeightType;

typedef struct {
    VLAlgorithm algorithm;
    VLWeightType weight;
    int include_geometry;  /* Include path coordinates in result */
    double max_distance;   /* Maximum search distance (0 = unlimited) */
    double max_duration;   /* Maximum search duration (0 = unlimited) */
    double epsilon;        /* Suboptimality bound (0 = optimal, 0.1 = up to 10% longer) */
    VLProfile profile;     /* Vehicle profile for edge filtering */
} VLRouteOptions;

/* ============================================================================
 * Route Result
 * ============================================================================ */

typedef struct {
    VLStatus status;

    /* Route metrics */
    double distance_m;     /* Total distance in meters */
    double duration_s;     /* Total duration in seconds */

    /* Path geometry (if requested) */
    int num_coords;
    VLCoord *coords;

    /* Path as node indices (always included) */
    int num_nodes;
    uint32_t *node_indices;

    /* Statistics */
    uint32_t nodes_explored;
    double search_time_ms;
} VLRoute;

/* ============================================================================
 * PBF Parsing Intermediate Structures
 * ============================================================================ */

/* Parsed OSM node (temporary, during parsing) */
typedef struct {
    int64_t id;
    double lat;
    double lon;
} VLOSMNode;

/* Parsed OSM way (temporary, during parsing) */
typedef struct {
    int64_t id;
    int64_t *node_refs;
    int num_refs;
    int highway_type;      /* Encoded road type */
    int oneway;            /* 1=yes, -1=reverse, 0=no */
    int max_speed;         /* km/h, 0=unknown */
    uint16_t access_flags; /* Access restriction flags */
} VLOSMWay;

/* PBF parsing context */
typedef struct {
    /* Parsed data */
    VLOSMNode *nodes;
    size_t num_nodes;
    size_t nodes_capacity;

    VLOSMWay *ways;
    size_t num_ways;
    size_t ways_capacity;

    /* Statistics */
    size_t total_nodes_parsed;
    size_t total_ways_parsed;
    size_t highway_ways_kept;
} VLPBFContext;

/* ============================================================================
 * Binary Min-Heap for Priority Queue (uses shared heap)
 * ============================================================================ */

typedef SHHeapEntry VLHeapEntry;

/* ============================================================================
 * Graph Building Context
 * ============================================================================ */

/* Temporary edge during graph construction */
typedef struct {
    uint32_t source;
    uint32_t target;
    uint32_t distance;     /* millimeters */
    uint16_t duration;     /* deciseconds */
    uint16_t flags;
} VLTempEdge;

/* Graph building context */
typedef struct {
    SHHashmapI64U32 *node_map;

    /* Nodes being built */
    VLNode *nodes;
    size_t num_nodes;
    size_t nodes_capacity;

    /* Temporary edges (unsorted) */
    VLTempEdge *temp_edges;
    size_t num_temp_edges;
    size_t temp_edges_capacity;
} VLGraphBuilder;

/* ============================================================================
 * Constants
 * ============================================================================ */

#define VL_EARTH_RADIUS_M 6371000.0
#define VL_INF 1e18
#define VL_INVALID_NODE UINT32_MAX

/* Default speeds by road type (km/h) */
#define VL_SPEED_MOTORWAY    110
#define VL_SPEED_TRUNK       90
#define VL_SPEED_PRIMARY     70
#define VL_SPEED_SECONDARY   60
#define VL_SPEED_TERTIARY    50
#define VL_SPEED_RESIDENTIAL 30
#define VL_SPEED_SERVICE     20
#define VL_SPEED_DEFAULT     50

/* ============================================================================
 * Binary File Format
 * ============================================================================ */

#define VL_BINARY_MAGIC 0x56454C47  /* "VELG" */
#define VL_BINARY_VERSION 2         /* v2: added reverse graph index */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_nodes;
    uint32_t num_edges;
    double bbox_min_lat;
    double bbox_min_lon;
    double bbox_max_lat;
    double bbox_max_lon;
    uint32_t reserved[8];
} VLBinaryHeader;

#ifdef __cplusplus
}
#endif

#endif /* VL_TYPES_H */
