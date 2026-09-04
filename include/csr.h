#ifndef CSR_H
#define CSR_H

#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>

#include "types.h"      // vid_t, eid_t.
#include "mtx.h"        // mtx_t, mtx_policy_t -- needed by csr_build_from_mtx.

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

static inline eid_t csr_degree(const csr_t *g, vid_t v) {
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
 * Whether an entry (i, j) needs to be duplicataed is decided
 * from mx->is_symmetric: a symmetric banner means the file
 * stores only one triangle, so both directions must be inserted.
 *
 * On success fills 'g' and returns 0. 'g' is then owned by the
 * caller and must eventually be released with csr_free(). On
 * failure returns -1 and 'g' is left untouched. */
int csr_build_from_mtx(const mtx_t *mx, csr_t *g);

#endif
