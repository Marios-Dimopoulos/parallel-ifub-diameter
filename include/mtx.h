#ifndef MTX_H
#define MTX_H

#include <stddef.h>
#include <stdint.h>
#include "types.h"

/* A memory-mapped Matrix Market file, with its header already parsed.
 * Nothing here owns heap memory: `base` points into a mapping created
 * by mmap(), and `data` points somewhere inside it. Reading through
 * these pointers is what triggers the actual disk I/O, page by page,
 * on demand. */
typedef struct {
    const char *base;       // Start of the mapping.
    size_t len;             // file size in bytes.
    const char *data;       // first data line.

    vid_t n;                // matrix dimension.
    uint64_t nnz_lines;     // entry count from the header line.

    int is_pattern;         // no third column of values.
    int is_symmetric;       // banner says only the triangle stored.
} mtx_t;

int mtx_open(const char *path, mtx_t *mx);

void mtx_close(mtx_t *mx);

void mtx_describe(const mtx_t *mx);

#endif