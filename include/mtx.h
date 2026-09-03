#ifndef MTX_H
#define MTX_H

#include <stddef.h>
#include <stdint.h>
#include "graph.h"

/* How the file stores an undirected graph.
 *
 * MTX_DUP:  the file holds only one triangle of the matrix, so entry
 *           (i,j) implies (j,i). We must insert BOTH directions.
 *           This is what a "symmetric" banner means.
 *
 * MTX_ASIS: the file already lists both directions explicitly. We
 *           insert each entry exactly once. This is a "general"
 *           banner holding an already-symmetrised graph. */
typedef enum {
    MTX_DUP,
    MTX_ASIS
} mtx_policy_t;

// A memory-mapped Matrix Market file, with its header already parsed.
// Nothing here owns heap memory: `base` points into a mapping created
// by mmap(), and `data` points somewhere inside it. Reading through
// these pointers is what triggers the actual disk I/O, page by page,
// on demand.
typedef struct {
    const char *base;       // Start of the mapping.
    size_t len;             // file size in bytes.
    const char *data;       // first data line, i.e. past the header

    vid_t n;                // matrix dimension (we require square).
    uint64_t nnz_lines;     // entry count from the header line.

    int is_pattern;         // no third column of values.
    int is_symmetric;       // banner says only the triangle stored.
} mtx_t;

// Opens and mmaps 'path', parses the banner and the dimension line,
// and leaves 'mx->data' pointing to the first entry.
// Returns 0 on success, -1 on failure.
int mtx_open(const char *path, mtx_t *mx);

// Unmaps the file. Safe to call twice. 
void mtx_close(mtx_t *mx);

// Prints size, dimensions and detected format to stderr.
void mtx_describe(const mtx_t *mx);

// The policy implied by the banner. Overridable from the command line.
mtx_policy_t mtx_policy(const mtx_t *mx);

#endif