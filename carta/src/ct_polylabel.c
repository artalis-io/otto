/*
 * ct_polylabel.c - Pole of Inaccessibility for Polygon Labeling
 *
 * Iterative cell subdivision to find the internal point with maximum
 * distance to any polygon edge. Uses sh_dheap as the priority queue.
 *
 * Based on the Mapbox polylabel algorithm (ISC license).
 */

#include "ct_polylabel.h"
#include "sh_dheap.h"
#include <math.h>
#include <float.h>

/* Cell in the subdivision grid */
typedef struct {
    double cx, cy;   /* Center */
    double half;     /* Half-size */
    double dist;     /* Distance from center to nearest edge */
    double max_dist; /* Maximum possible distance in this cell */
} PolyCell;

/* Comparator: higher max_dist = higher priority (max-heap) */
static int cell_compare(const void *a, const void *b)
{
    const PolyCell *ca = (const PolyCell *)a;
    const PolyCell *cb = (const PolyCell *)b;
    if (ca->max_dist > cb->max_dist) return 1;
    if (ca->max_dist < cb->max_dist) return -1;
    return 0;
}

/*
 * Signed distance from point to polygon edges.
 * Negative if outside, positive if inside.
 */
static double point_to_polygon_dist(double px, double py,
                                     const CTCoord **rings,
                                     const int *ring_sizes,
                                     int num_rings)
{
    int inside = 0;
    double min_dist_sq = DBL_MAX;

    for (int r = 0; r < num_rings; r++) {
        const CTCoord *ring = rings[r];
        int len = ring_sizes[r];

        for (int i = 0, j = len - 1; i < len; j = i++) {
            double ax = ring[i].lon, ay = ring[i].lat;
            double bx = ring[j].lon, by = ring[j].lat;

            /* Ray casting for inside/outside test */
            if ((ay > py) != (by > py) &&
                (px < (bx - ax) * (py - ay) / (by - ay) + ax)) {
                inside = !inside;
            }

            /* Distance from point to segment */
            double dx = bx - ax;
            double dy = by - ay;
            double seg_len_sq = dx * dx + dy * dy;

            double t;
            if (seg_len_sq < 1e-12) {
                t = 0;
            } else {
                t = ((px - ax) * dx + (py - ay) * dy) / seg_len_sq;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
            }

            double proj_x = ax + t * dx;
            double proj_y = ay + t * dy;
            double d_sq = (px - proj_x) * (px - proj_x) +
                          (py - proj_y) * (py - proj_y);

            if (d_sq < min_dist_sq) {
                min_dist_sq = d_sq;
            }
        }
    }

    double min_dist = sqrt(min_dist_sq);
    return inside ? min_dist : -min_dist;
}

/* Create a cell and compute its properties */
static PolyCell make_cell(double cx, double cy, double half,
                           const CTCoord **rings, const int *ring_sizes,
                           int num_rings)
{
    PolyCell cell;
    cell.cx = cx;
    cell.cy = cy;
    cell.half = half;
    cell.dist = point_to_polygon_dist(cx, cy, rings, ring_sizes, num_rings);
    /* Maximum possible distance in this cell = dist + half * sqrt(2) */
    cell.max_dist = cell.dist + half * 1.4142135623730951;
    return cell;
}

int ct_polylabel_with_holes(const CTCoord **rings, const int *ring_sizes,
                             int num_rings, double precision,
                             double *out_x, double *out_y, double *out_dist)
{
    if (!rings || !ring_sizes || num_rings < 1 || ring_sizes[0] < 3) {
        return 0;
    }

    /* Find bounding box of outer ring */
    const CTCoord *outer = rings[0];
    int outer_len = ring_sizes[0];

    double min_x = outer[0].lon, max_x = outer[0].lon;
    double min_y = outer[0].lat, max_y = outer[0].lat;
    for (int i = 1; i < outer_len; i++) {
        if (outer[i].lon < min_x) min_x = outer[i].lon;
        if (outer[i].lon > max_x) max_x = outer[i].lon;
        if (outer[i].lat < min_y) min_y = outer[i].lat;
        if (outer[i].lat > max_y) max_y = outer[i].lat;
    }

    double width = max_x - min_x;
    double height = max_y - min_y;
    double cell_size = (width > height ? width : height);

    if (cell_size < 1e-12) return 0;

    double half = cell_size / 2.0;

    /* Create priority queue */
    SHDHeap heap;
    sh_dheap_init(&heap, sizeof(PolyCell), cell_compare);

    /* Seed grid with initial cells */
    for (double x = min_x; x < max_x; x += cell_size) {
        for (double y = min_y; y < max_y; y += cell_size) {
            PolyCell c = make_cell(x + half, y + half, half,
                                   rings, ring_sizes, num_rings);
            sh_dheap_push(&heap, &c);
        }
    }

    /* Best cell found so far */
    PolyCell best_cell = make_cell(min_x + width / 2.0, min_y + height / 2.0, 0,
                                   rings, ring_sizes, num_rings);

    /* Centroid as initial candidate */
    double sum_area = 0, sum_x = 0, sum_y = 0;
    for (int i = 0, j = outer_len - 1; i < outer_len; j = i++) {
        double cross = outer[i].lon * outer[j].lat - outer[j].lon * outer[i].lat;
        sum_area += cross;
        sum_x += (outer[i].lon + outer[j].lon) * cross;
        sum_y += (outer[i].lat + outer[j].lat) * cross;
    }
    if (fabs(sum_area) > 1e-12) {
        sum_area *= 3.0;
        PolyCell centroid_cell = make_cell(sum_x / sum_area, sum_y / sum_area, 0,
                                           rings, ring_sizes, num_rings);
        if (centroid_cell.dist > best_cell.dist) {
            best_cell = centroid_cell;
        }
    }

    /* Subdivide until we reach desired precision */
    int iterations = 0;
    int max_iterations = 10000;

    while (!sh_dheap_empty(&heap) && iterations < max_iterations) {
        PolyCell cell;
        if (!sh_dheap_pop(&heap, &cell)) break;
        iterations++;

        /* Update best if this cell's center is better */
        if (cell.dist > best_cell.dist) {
            best_cell = cell;
        }

        /* Skip if this cell can't contain a better solution */
        if (cell.max_dist - best_cell.dist <= precision) {
            continue;
        }

        /* Subdivide into 4 children */
        double h = cell.half / 2.0;
        PolyCell c1 = make_cell(cell.cx - h, cell.cy - h, h, rings, ring_sizes, num_rings);
        PolyCell c2 = make_cell(cell.cx + h, cell.cy - h, h, rings, ring_sizes, num_rings);
        PolyCell c3 = make_cell(cell.cx - h, cell.cy + h, h, rings, ring_sizes, num_rings);
        PolyCell c4 = make_cell(cell.cx + h, cell.cy + h, h, rings, ring_sizes, num_rings);

        sh_dheap_push(&heap, &c1);
        sh_dheap_push(&heap, &c2);
        sh_dheap_push(&heap, &c3);
        sh_dheap_push(&heap, &c4);
    }

    sh_dheap_free(&heap);

    if (out_x) *out_x = best_cell.cx;
    if (out_y) *out_y = best_cell.cy;
    if (out_dist) *out_dist = best_cell.dist;

    return best_cell.dist > 0 ? 1 : 0;
}

int ct_polylabel(const CTCoord *coords, int num_coords,
                 double precision,
                 double *out_x, double *out_y, double *out_dist)
{
    if (!coords || num_coords < 3) return 0;

    const CTCoord *rings[1] = { coords };
    int ring_sizes[1] = { num_coords };

    return ct_polylabel_with_holes(rings, ring_sizes, 1, precision,
                                   out_x, out_y, out_dist);
}
