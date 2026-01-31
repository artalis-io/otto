/*
 * debug_pbf.c - Debug PBF tag parsing
 */

#include "locus.h"
#include "sh_protobuf.h"
#include "sh_inflate.h"
#include "sh_pbf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PBF field numbers */
#define PBF_PRIMITIVEBLOCK_STRINGTABLE  1
#define PBF_PRIMITIVEBLOCK_PRIMITIVEGROUP 2
#define PBF_PRIMITIVEGROUP_DENSE        2
#define PBF_DENSENODES_ID               1
#define PBF_DENSENODES_LAT              8
#define PBF_DENSENODES_LON              9
#define PBF_DENSENODES_KEYS_VALS        10

static void dump_string_table(const uint8_t *data, size_t len)
{
    SHStringTable st;
    sh_string_table_init(&st);
    sh_string_table_parse(&st, data, len);

    printf("String table (%zu strings):\n", st.count);
    for (size_t i = 0; i < st.count && i < 50; i++) {
        const char *s = sh_string_table_get(&st, i);
        if (s && strlen(s) > 0) {
            printf("  [%zu] = \"%s\"\n", i, s);
        }
    }
    if (st.count > 50) {
        printf("  ... and %zu more\n", st.count - 50);
    }

    sh_string_table_free(&st);
}

static void dump_dense_tags(const uint8_t *data, size_t len, const SHStringTable *st)
{
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    uint64_t *keys_vals = NULL;  /* Unsigned - string table indices */
    size_t kv_count = 0;

    /* Find keys_vals field */
    while (ptr < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(ptr, end - ptr, &field, &wire);
        if (n <= 0) break;
        ptr += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(ptr, end - ptr, &field_len);
            if (n <= 0) break;
            ptr += n;

            if (field == PBF_DENSENODES_KEYS_VALS) {
                kv_count = sh_pb_count_packed_varint(ptr, field_len);
                keys_vals = malloc(kv_count * sizeof(uint64_t));
                if (keys_vals) {
                    /* Use unsigned varint reader - keys_vals are string table indices */
                    sh_pb_read_packed_varint_array(ptr, field_len, keys_vals, kv_count);
                }
            }
            ptr += field_len;
        } else {
            n = sh_pb_skip_field(ptr, end - ptr, wire);
            if (n <= 0) break;
            ptr += n;
        }
    }

    if (!keys_vals || kv_count == 0) {
        printf("  No tags found in DenseNodes\n");
        return;
    }

    printf("  DenseNodes keys_vals (%zu values):\n", kv_count);

    /* Parse tags for first few nodes */
    int node_idx = 0;
    size_t i = 0;
    while (i < kv_count && node_idx < 5) {
        printf("    Node %d tags:\n", node_idx);
        int tag_count = 0;
        while (i < kv_count && keys_vals[i] != 0) {
            uint64_t key_idx = keys_vals[i];
            uint64_t val_idx = keys_vals[i + 1];
            const char *key = sh_string_table_get(st, (size_t)key_idx);
            const char *val = sh_string_table_get(st, (size_t)val_idx);
            printf("      %s=%s (indices %lu, %lu)\n",
                   key ? key : "(null)", val ? val : "(null)",
                   (unsigned long)key_idx, (unsigned long)val_idx);
            i += 2;
            tag_count++;
            if (tag_count > 10) {
                printf("      ... more tags\n");
                while (i < kv_count && keys_vals[i] != 0) i += 2;
                break;
            }
        }
        if (i < kv_count) i++;  /* Skip 0 delimiter */
        node_idx++;
    }

    free(keys_vals);
}

int main(int argc, char **argv)
{
    const char *filename = argc > 1 ? argv[1] : "../data/monaco-latest.osm.pbf";

    printf("Debug PBF: %s\n\n", filename);

    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open file\n");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc((size_t)file_size);
    fread(data, 1, (size_t)file_size, f);
    fclose(f);

    const uint8_t *ptr = data;
    const uint8_t *end = data + file_size;
    int block_num = 0;

    while (ptr < end && block_num < 3) {
        /* Read blob header length */
        if (ptr + 4 > end) break;
        uint32_t header_len = ((uint32_t)ptr[0] << 24) |
                              ((uint32_t)ptr[1] << 16) |
                              ((uint32_t)ptr[2] << 8) |
                              ((uint32_t)ptr[3]);
        ptr += 4;

        if (ptr + header_len > end) break;

        /* Parse blob header */
        char type[32] = {0};
        uint32_t data_size = 0;
        size_t consumed;
        sh_pbf_parse_blob_header(ptr, header_len, type, sizeof(type), &data_size, &consumed);
        ptr += header_len;

        if (ptr + data_size > end) break;

        printf("Block %d: type=%s, size=%u\n", block_num, type, data_size);

        if (strcmp(type, "OSMData") == 0) {
            SHPBFBlob blob;
            if (sh_pbf_decompress_blob(ptr, data_size, &blob) == SH_OK) {
                /* Parse PrimitiveBlock to find string table */
                const uint8_t *bp = blob.data;
                const uint8_t *bend = blob.data + blob.len;

                SHStringTable st;
                sh_string_table_init(&st);

                while (bp < bend) {
                    uint32_t field, wire;
                    int n = sh_pb_read_tag(bp, bend - bp, &field, &wire);
                    if (n <= 0) break;
                    bp += n;

                    if (wire == SH_PB_WIRE_LENGTH_DELIM) {
                        uint64_t field_len;
                        n = sh_pb_read_varint(bp, bend - bp, &field_len);
                        if (n <= 0) break;
                        bp += n;

                        if (field == PBF_PRIMITIVEBLOCK_STRINGTABLE) {
                            printf("  Found string table (%lu bytes)\n", (unsigned long)field_len);
                            dump_string_table(bp, field_len);
                            sh_string_table_parse(&st, bp, field_len);
                        } else if (field == PBF_PRIMITIVEBLOCK_PRIMITIVEGROUP) {
                            printf("  Found primitive group (%lu bytes)\n", (unsigned long)field_len);
                            /* Look for DenseNodes */
                            const uint8_t *gp = bp;
                            const uint8_t *gend = bp + field_len;
                            while (gp < gend) {
                                uint32_t gfield, gwire;
                                int gn = sh_pb_read_tag(gp, gend - gp, &gfield, &gwire);
                                if (gn <= 0) break;
                                gp += gn;

                                if (gwire == SH_PB_WIRE_LENGTH_DELIM) {
                                    uint64_t glen;
                                    gn = sh_pb_read_varint(gp, gend - gp, &glen);
                                    if (gn <= 0) break;
                                    gp += gn;

                                    if (gfield == PBF_PRIMITIVEGROUP_DENSE) {
                                        printf("  Found DenseNodes (%lu bytes)\n", (unsigned long)glen);
                                        dump_dense_tags(gp, glen, &st);
                                    }
                                    gp += glen;
                                } else {
                                    gn = sh_pb_skip_field(gp, gend - gp, gwire);
                                    if (gn <= 0) break;
                                    gp += gn;
                                }
                            }
                        }
                        bp += field_len;
                    } else {
                        n = sh_pb_skip_field(bp, bend - bp, wire);
                        if (n <= 0) break;
                        bp += n;
                    }
                }

                sh_string_table_free(&st);
                sh_pbf_blob_free(&blob);
            }
        }

        ptr += data_size;
        block_num++;
        printf("\n");
    }

    free(data);
    return 0;
}
