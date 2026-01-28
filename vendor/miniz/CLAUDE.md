# Miniz Compression Library - Claude Skills

## Overview

Miniz is a public domain, single-source-file DEFLATE/zlib compression library. It provides a drop-in replacement for a large subset of zlib's API plus additional features for ZIP archive handling.

**Key features:**
- zlib-compatible API (drop-in replacement)
- DEFLATE compression/decompression
- ZIP archive reading/writing
- CRC-32 and Adler-32 checksums
- No external dependencies
- WASM-compatible

**License:** Public domain (Unlicense)

## Quick Start

### Simple Compression

```c
#include "miniz.h"

// Compress data
mz_ulong src_len = strlen(source_data);
mz_ulong cmp_len = mz_compressBound(src_len);
unsigned char *compressed = malloc(cmp_len);

if (mz_compress(compressed, &cmp_len, (unsigned char*)source_data, src_len) == MZ_OK) {
    printf("Compressed %lu -> %lu bytes\n", src_len, cmp_len);
}

// Decompress data
mz_ulong dst_len = src_len;  // Must know original size
unsigned char *decompressed = malloc(dst_len);

if (mz_uncompress(decompressed, &dst_len, compressed, cmp_len) == MZ_OK) {
    printf("Decompressed successfully\n");
}

free(compressed);
free(decompressed);
```

### Streaming Compression

```c
mz_stream stream;
memset(&stream, 0, sizeof(stream));

if (mz_deflateInit(&stream, MZ_DEFAULT_COMPRESSION) != MZ_OK) {
    return -1;
}

stream.next_in = input_data;
stream.avail_in = input_size;
stream.next_out = output_buffer;
stream.avail_out = output_capacity;

int status;
do {
    status = mz_deflate(&stream, MZ_FINISH);
} while (status == MZ_OK);

if (status == MZ_STREAM_END) {
    size_t compressed_size = stream.total_out;
}

mz_deflateEnd(&stream);
```

### Streaming Decompression

```c
mz_stream stream;
memset(&stream, 0, sizeof(stream));

if (mz_inflateInit(&stream) != MZ_OK) {
    return -1;
}

stream.next_in = compressed_data;
stream.avail_in = compressed_size;
stream.next_out = output_buffer;
stream.avail_out = output_capacity;

int status;
do {
    status = mz_inflate(&stream, MZ_FINISH);
} while (status == MZ_OK);

if (status == MZ_STREAM_END) {
    size_t decompressed_size = stream.total_out;
}

mz_inflateEnd(&stream);
```

## Core Types

### mz_stream

Compression/decompression stream state:

```c
typedef struct mz_stream_s {
    const unsigned char *next_in;   // Next input byte
    unsigned int avail_in;          // Bytes available at next_in
    mz_ulong total_in;              // Total bytes consumed

    unsigned char *next_out;        // Next output byte
    unsigned int avail_out;         // Bytes available at next_out
    mz_ulong total_out;             // Total bytes produced

    mz_alloc_func zalloc;           // Optional allocator
    mz_free_func zfree;             // Optional free function
    void *opaque;                   // User data for alloc/free
} mz_stream;
```

## Compression Levels

```c
MZ_NO_COMPRESSION      // 0 - No compression (store only)
MZ_BEST_SPEED          // 1 - Fastest compression
MZ_BEST_COMPRESSION    // 9 - Best compression ratio
MZ_UBER_COMPRESSION    // 10 - Maximum compression (slow)
MZ_DEFAULT_LEVEL       // 6 - Good balance
MZ_DEFAULT_COMPRESSION // -1 - Same as MZ_DEFAULT_LEVEL
```

## Return Codes

```c
MZ_OK            // 0 - Success
MZ_STREAM_END    // 1 - End of stream reached
MZ_NEED_DICT     // 2 - Dictionary needed (unused)
MZ_ERRNO         // -1 - I/O error
MZ_STREAM_ERROR  // -2 - Invalid stream state
MZ_DATA_ERROR    // -3 - Invalid/corrupted data
MZ_MEM_ERROR     // -4 - Out of memory
MZ_BUF_ERROR     // -5 - Buffer too small
MZ_VERSION_ERROR // -6 - Version mismatch
MZ_PARAM_ERROR   // -10000 - Invalid parameter
```

## Flush Modes

```c
MZ_NO_FLUSH      // 0 - Normal operation
MZ_PARTIAL_FLUSH // 1 - Flush some output
MZ_SYNC_FLUSH    // 2 - Flush all output
MZ_FULL_FLUSH    // 3 - Full flush (reset state)
MZ_FINISH        // 4 - Finish compression
```

## Low-Level API (tinfl)

For decompression with more control. Used by this project for OSM PBF parsing.

### Memory-to-Memory Decompression

```c
#include "miniz_tinfl.h"

// Decompress to pre-allocated buffer
size_t result = tinfl_decompress_mem_to_mem(
    output_buffer,    // Destination
    output_capacity,  // Destination size
    compressed_data,  // Source
    compressed_size,  // Source size
    0                 // Flags (0 for raw deflate)
);

if (result == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    // Decompression failed
}
```

### Decompress to Heap

```c
size_t decompressed_size;
void *data = tinfl_decompress_mem_to_heap(
    compressed_data,
    compressed_size,
    &decompressed_size,
    TINFL_FLAG_PARSE_ZLIB_HEADER  // For zlib-wrapped data
);

if (data) {
    // Use decompressed data
    mz_free(data);
}
```

### Decompression Flags

```c
TINFL_FLAG_PARSE_ZLIB_HEADER           // 1 - Input has zlib header
TINFL_FLAG_HAS_MORE_INPUT              // 2 - More input available
TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF // 4 - Output buffer holds entire result
TINFL_FLAG_COMPUTE_ADLER32             // 8 - Compute checksum
```

## Checksum Functions

```c
// CRC-32 (used by gzip, PNG, ZIP)
mz_ulong crc = MZ_CRC32_INIT;
crc = mz_crc32(crc, data, data_len);

// Adler-32 (used by zlib)
mz_ulong adler = MZ_ADLER32_INIT;
adler = mz_adler32(adler, data, data_len);
```

## ZIP Archive API

### Reading ZIP Files

```c
#include "miniz_zip.h"

mz_zip_archive zip;
memset(&zip, 0, sizeof(zip));

if (mz_zip_reader_init_file(&zip, "archive.zip", 0)) {
    int num_files = mz_zip_reader_get_num_files(&zip);

    for (int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        mz_zip_reader_file_stat(&zip, i, &stat);
        printf("File: %s (%lu bytes)\n", stat.m_filename, stat.m_uncomp_size);
    }

    // Extract file to heap
    size_t size;
    void *data = mz_zip_reader_extract_file_to_heap(&zip, "file.txt", &size, 0);
    if (data) {
        // Use data
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
}
```

### Writing ZIP Files

```c
mz_zip_archive zip;
memset(&zip, 0, sizeof(zip));

if (mz_zip_writer_init_file(&zip, "output.zip", 0)) {
    // Add file from memory
    mz_zip_writer_add_mem(&zip, "hello.txt", "Hello World!", 12,
                          MZ_DEFAULT_COMPRESSION);

    // Add file from disk
    mz_zip_writer_add_file(&zip, "data.bin", "local_file.bin", NULL, 0,
                           MZ_DEFAULT_COMPRESSION);

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
}
```

### Extract Single File (Convenience)

```c
// Extract directly from archive file to heap
size_t size;
void *data = mz_zip_extract_archive_file_to_heap("archive.zip", "file.txt",
                                                  &size, 0);
if (data) {
    // Use data
    mz_free(data);
}
```

## Common Patterns

### Raw DEFLATE (No Header)

Used for OSM PBF blob decompression:

```c
// Compression without zlib header
mz_stream stream;
memset(&stream, 0, sizeof(stream));
mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                -MZ_DEFAULT_WINDOW_BITS,  // Negative = raw deflate
                9, MZ_DEFAULT_STRATEGY);

// Decompression without zlib header
mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS);
```

### PNG Compression

Miniz includes a simple PNG writer:

```c
// Write RGBA image to PNG in memory
size_t png_size;
void *png_data = tdefl_write_image_to_png_file_in_memory(
    rgba_pixels,    // Image data
    width, height,  // Dimensions
    4,              // Bytes per pixel (RGBA)
    &png_size       // Output size
);

if (png_data) {
    // Write png_data to file
    mz_free(png_data);
}
```

### Incremental Decompression

For processing large streams:

```c
tinfl_decompressor decomp;
tinfl_init(&decomp);

mz_uint8 dict[TINFL_LZ_DICT_SIZE];
size_t dict_ofs = 0;

while (have_more_input) {
    size_t in_bytes = input_available;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;

    tinfl_status status = tinfl_decompress(&decomp,
        input_ptr, &in_bytes,
        dict, dict + dict_ofs, &out_bytes,
        have_more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0);

    // Process dict[dict_ofs .. dict_ofs + out_bytes]
    input_ptr += in_bytes;
    dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);

    if (status == TINFL_STATUS_DONE) break;
    if (status < 0) { /* error */ break; }
}
```

## Compile Flags

```c
#define MINIZ_NO_STDIO           // Disable file I/O
#define MINIZ_NO_TIME            // Disable time functions
#define MINIZ_NO_ARCHIVE_APIS    // Disable ZIP support
#define MINIZ_NO_ZLIB_APIS       // Disable zlib compatibility
#define MINIZ_NO_MALLOC          // Disable dynamic allocation
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES  // Don't define zlib names
```

## Building

```makefile
# Compile as object files
gcc -c miniz.c miniz_tdef.c miniz_tinfl.c -O2

# Link with your project
gcc -o myapp myapp.c miniz.o miniz_tdef.o miniz_tinfl.o
```

## zlib Compatibility

Miniz provides zlib-compatible names by default:

```c
// These work as drop-in zlib replacements
compress(dest, &dest_len, src, src_len);
uncompress(dest, &dest_len, src, src_len);
deflateInit(&stream, level);
inflateInit(&stream);
crc32(crc, data, len);
```

To disable (if using with real zlib):
```c
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
```

## Resources

- **Repository:** https://github.com/richgel999/miniz
- **API Docs:** See header comments in miniz.h
- **RFC 1950:** zlib format specification
- **RFC 1951:** DEFLATE compression specification
