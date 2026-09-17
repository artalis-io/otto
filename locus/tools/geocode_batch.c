/*
 * geocode_batch — batch forward-geocode with optional CITY-ANCHORED bbox
 * constraint and a forward<->reverse round-trip.
 *
 * Reads tab-separated lines from stdin: "id\tstreet_query[\tcity]".
 * If a city is given, geocoding is two-stage and stays INDEPENDENT of any other
 * source: (1) geocode the city -> centroid, (2) search the street constrained to
 * a bbox around that centroid (both via opts.bounds and an explicit post-filter,
 * so it works regardless of the index path). This fixes Locus's postcode-blind
 * fuzzy matching that otherwise snaps a street name to the wrong town.
 *
 * Emits one JSON object per line: best in-box match coord/class/score, runner-up
 * score (ambiguity), the match's postcode/country_code, a reverse round-trip
 * (country + distance), and whether the bbox constraint was applied/satisfied.
 *
 *   geocode_batch <index.idx|file.osm.pbf>  < queries.tsv  > results.jsonl
 */
#include "../include/locus.h"
#include "../include/lc_index.h"
#include "../include/lc_types.h"
#include "../include/lc_serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Equality after normalizing to lowercase alphanumerics. Used for house numbers
 * ("1." == "1", "1/A" == "1a") and postcodes ("949 01" == "94901"). */
static int alnum_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    char na[32], nb[32]; int i, j;
    for (i = j = 0; a[i] && j < 31; i++) if (isalnum((unsigned char)a[i])) na[j++] = (char)tolower((unsigned char)a[i]);
    na[j] = '\0';
    for (i = j = 0; b[i] && j < 31; i++) if (isalnum((unsigned char)b[i])) nb[j++] = (char)tolower((unsigned char)b[i]);
    nb[j] = '\0';
    return na[0] && nb[0] && strcmp(na, nb) == 0;
}

static void json_str(FILE *o, const char *s) {
    fputc('"', o);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", o); break;
        case '\\': fputs("\\\\", o); break;
        case '\n': fputs("\\n", o);  break;
        case '\r': fputs("\\r", o);  break;
        case '\t': fputs("\\t", o);  break;
        default:
            if (*p < 0x20) fprintf(o, "\\u%04x", *p);
            else fputc(*p, o);
        }
    }
    fputc('"', o);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <index.idx|file.osm.pbf> < id\\tquery\n", argv[0]); return 2; }

    LCIndex *index = NULL;
    if (lc_is_binary_index(argv[1])) {
        index = lc_index_load(argv[1]);
    } else {
        index = lc_index_create();
        if (index && lc_index_build_from_pbf(index, argv[1], NULL) != LC_OK) {
            lc_index_free(index); index = NULL;
        }
    }
    if (!index) { fprintf(stderr, "failed to load index: %s\n", argv[1]); return 1; }
    fprintf(stderr, "loaded %u entities\n", lc_index_entity_count(index));

    LCReverseOptions ropt; lc_reverse_options_default(&ropt); ropt.radius_m = 2000.0;

    /* bbox half-extent around a city centroid: ~28 km lat, wider lon */
    const double DLAT = 0.25, DLON = 0.38;

    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, stdin)) != -1) {
        if (n && line[n-1] == '\n') line[--n] = '\0';
        if (n == 0) continue;
        /* split id \t street \t city \t housenumber \t postcode */
        char *t1 = strchr(line, '\t');
        const char *id = "", *query = line, *city = "", *hnum = "", *pcode = "";
        if (t1) {
            *t1 = '\0'; id = line; query = t1 + 1;
            char *t2 = strchr(t1 + 1, '\t');
            if (t2) {
                *t2 = '\0'; city = t2 + 1;
                char *t3 = strchr(t2 + 1, '\t');
                if (t3) {
                    *t3 = '\0'; hnum = t3 + 1;
                    char *t4 = strchr(t3 + 1, '\t');
                    if (t4) { *t4 = '\0'; pcode = t4 + 1; }
                }
            }
        }

        /* stage 1: anchor bbox on the city centroid (independent of query) */
        int have_box = 0; SHBBox box = {0,0,0,0}; double city_lat=0, city_lon=0;
        if (city && *city) {
            LCSearchOptions co; lc_search_options_default(&co); co.limit = 3;
            LCSearchResult cr;
            if (lc_search(index, city, &co, &cr) == LC_OK && cr.num_results > 0) {
                const LCEntity *ce = lc_search_get_entity(index, &cr.matches[0]);
                if (ce) {
                    city_lat = ce->centroid.lat; city_lon = ce->centroid.lon;
                    box.min_lat = city_lat - DLAT; box.max_lat = city_lat + DLAT;
                    box.min_lon = city_lon - DLON; box.max_lon = city_lon + DLON;
                    have_box = 1;
                }
            }
            if (cr.matches) lc_search_result_free(&cr);
        }

        /* stage 2: street search, constrained to the bbox when we have one.
         * Larger limit so low-ranked ADDRESS entities (importance 0.05, indexed
         * by street name only) are in the candidate set for house-number matching. */
        LCSearchOptions sopt; lc_search_options_default(&sopt);
        sopt.limit = 200;   /* common HU street names have 100+ ADDRESS entities; need the whole set to postcode-filter */
        if (have_box) sopt.bounds = &box;

        LCSearchResult res;
        if (lc_search(index, query, &sopt, &res) != LC_OK || res.num_results == 0) {
            fputs("{\"id\":", stdout); json_str(stdout, id);
            fputs(",\"query\":", stdout); json_str(stdout, query);
            fprintf(stdout, ",\"found\":false,\"bbox\":%d}\n", have_box);
            if (res.matches) lc_search_result_free(&res);
            continue;
        }

        /* post-filter over the in-bbox candidates, in descending trust:
         *   1. ADDRESS with matching postcode AND house number (house + right town)
         *   2. ADDRESS with matching postcode           (right town, on the street)
         *   3. ADDRESS with matching house number       (right number, town unverified)
         *   4. best-scoring in-box result               (street/centroid)
         * Postcode disambiguates the common HU street name that repeats across many
         * settlements inside the bbox. */
        #define IN_BOX(ei) (!have_box || ((ei)->centroid.lat >= box.min_lat && (ei)->centroid.lat <= box.max_lat && \
                                          (ei)->centroid.lon >= box.min_lon && (ei)->centroid.lon <= box.max_lon))
        int c_hnpc=-1, c_pc=-1, c_hn=-1, c_box=-1;
        for (size_t i = 0; i < res.num_results; i++) {
            const LCEntity *ei = lc_search_get_entity(index, &res.matches[i]);
            if (!ei || !IN_BOX(ei)) continue;
            if (c_box < 0) c_box = (int)i;                 /* first in-box = best score */
            if (ei->fclass != LC_CLASS_ADDRESS) continue;
            int pcm = pcode[0] && alnum_eq(ei->address.postcode, pcode);
            int hnm = hnum[0]  && alnum_eq(ei->address.housenumber, hnum);
            if (pcm && hnm && c_hnpc < 0) c_hnpc = (int)i;
            if (pcm && c_pc < 0) c_pc = (int)i;
            if (hnm && c_hn < 0) c_hn = (int)i;
        }
        #undef IN_BOX
        int best = c_hnpc>=0 ? c_hnpc : c_pc>=0 ? c_pc : c_hn>=0 ? c_hn : (c_box>=0 ? c_box : 0);
        const LCEntity *chosen = lc_search_get_entity(index, &res.matches[best]);
        int in_box  = (best == c_box) || (best == c_hnpc) || (best == c_pc) || (best == c_hn) ? 1 : (have_box ? 0 : 1);
        int hn_match = chosen && chosen->fclass==LC_CLASS_ADDRESS && hnum[0]  && alnum_eq(chosen->address.housenumber, hnum);
        int pc_match = chosen && chosen->fclass==LC_CLASS_ADDRESS && pcode[0] && alnum_eq(chosen->address.postcode, pcode);

        const LCEntity *e = lc_search_get_entity(index, &res.matches[best]);
        double score  = res.matches[best].score;
        double score2 = res.num_results > 1 ? res.matches[best == 0 ? 1 : 0].score : 0.0;

        /* reverse round-trip on the returned point */
        const char *rev_cc = ""; double rev_dist = -1.0;
        if (e) {
            LCReverseResult rev;
            if (lc_reverse(index, e->centroid, &ropt, &rev) == LC_OK) {
                rev_dist = rev.distance_m;
                if (rev.hierarchy && rev.hierarchy_depth > 0 && rev.hierarchy[0])
                    rev_cc = rev.hierarchy[0]->address.country_code
                             ? rev.hierarchy[0]->address.country_code : "";
                lc_reverse_result_free(&rev);
            }
        }

        fputs("{\"id\":", stdout); json_str(stdout, id);
        fputs(",\"query\":", stdout); json_str(stdout, query);
        fputs(",\"found\":true", stdout);
        if (e) {
            fprintf(stdout, ",\"lat\":%.6f,\"lon\":%.6f", e->centroid.lat, e->centroid.lon);
            fputs(",\"class\":", stdout); json_str(stdout, lc_class_string(e->fclass));
            fputs(",\"postcode\":", stdout); json_str(stdout, e->address.postcode ? e->address.postcode : "");
            fputs(",\"cc\":", stdout); json_str(stdout, e->address.country_code ? e->address.country_code : "");
        }
        fprintf(stdout, ",\"score\":%.4f,\"score2\":%.4f", score, score2);
        fputs(",\"rev_cc\":", stdout); json_str(stdout, rev_cc);
        fprintf(stdout, ",\"rev_dist_m\":%.1f", rev_dist);
        fprintf(stdout, ",\"bbox\":%d,\"in_box\":%d,\"hn_match\":%d,\"pc_match\":%d", have_box, in_box, hn_match, pc_match);
        if (have_box) fprintf(stdout, ",\"city_lat\":%.6f,\"city_lon\":%.6f", city_lat, city_lon);
        fputs("}\n", stdout);

        lc_search_result_free(&res);
    }
    free(line);
    lc_index_free(index);
    return 0;
}
