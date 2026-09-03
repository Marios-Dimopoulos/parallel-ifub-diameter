#ifndef CSR_H
#define CSR_H

#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>

#include "mtx.h"        // mtx_t, mtx_policy_t -- needed by csr_build_from_mtx.

/* Vertex id. 32 bits is enough: the largest graph we target has
 * ~130 million vertices, well under UINT32_MAX (~4.3 billion).
 * 
 * Kept at 32 bits rather than 64 because col_idx has one entry per 
 * directed edge -- billions of them. Halving the bytes moved matters 
 * a lot on a memory-bandwidth-bound workload like BFS. */
typedef uint32_t vid_t;

/* Edge index. This one MUST be 64 bits, because the graphs i will be using,
 * will probably have more than ~2.3 billion nnz, which means overflow. */
typedef uint64_t eid_t;

#define PRIvid PRIu32
#define PREeid PRIu64
#define VID_MAX UINT32_MAX

/* Compressed Sparse Row storage for an undirected, unweighted graph.
 *
 * The neighbours of vertex v live in col_idx at positions
 *      row_ptr[v] .. row_ptr[v+1] - 1
 * so deg(v) = row_ptr[v+1] - row_ptr[v]. row_ptr has n+1 entries so
 * the last vertex needs no special case.
 *
 * The graph is undirected, so every edge {u, v} appears twice: once
 * as v in u's list, once as u in v's list. Hence m == 2 * |E|. */
typedef struct {
    vid_t n;
    eid_t m;
    eid_t *row_ptr;
    vid_t *col_idx;
} csr_t;

static inline eid_t car_degree(const csr_t *g, vid_t v) {
    return g->row_ptr[v + 1] - g->row_ptr[v];
}

static inline eid_t csr_begin(const csr_t *g, vid_t v) {
    return g->row_ptr[v];
}

static inline eid_t csr_end(const csr_t *g, vid_t v) {
    return g->row_ptr[v + 1];
}

/* Releases both arrays and zeroes the struct. Safe to call twice. */
void csr_free(csr_t *g);

/* Builds a csr_t from an already-opened Matrix Market file, using a
 * two-pass strategy:
 * 
 *      pass 1: count each vertex's degree
 *      prefix sum: turn degress into row_ptr
 *      pass 2: place each neigbour into its slot in col_idx
 *
 * 'pol' decides whether entry (i, j) becomes one directed edge or two
 * -- see mtx_policy_t in mtx.h. 
 *
 * On success fills 'g' and returns 0. 'g' is then owned by the
 * caller and must eventually be released with csr_free(). On
 * failure returns -1 and 'g' is left untouched. */
int csr_build_from_mtx(const mtx_t *mx, mtx_policy_t pol, csr_t *g);

#endif
