#ifndef CSR_H
#define CSR_H

#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>

#include "types.h"      
#include "mtx.h"        

/* Compressed Sparse Row storage for an undirected graph.
 *
 * The neighbours of vertex v live in col_idx at positions
 *      row_ptr[v] .. row_ptr[v+1] - 1
 * so deg(v) = row_ptr[v+1] - row_ptr[v]. row_ptr has n+1 entries so
 * the last vertex needs no special case. */
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

void csr_free(csr_t *g);

int csr_build_from_mtx(const mtx_t *mx, csr_t *g);

#endif
