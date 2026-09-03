#ifndef GRAPH_H
#define GRAPH_H

#include <stdint.h>     // uint32_t, uint64_t: fixed-width integer types
#include <inttypes.h>   // PRIu32, PRIu64: portable printf format macros
#include <stddef.h>     // size_t: unsigned integer type for sizes

// Vertex id. 32 bits is enough. The largest graph that is targeted
// has ~135 million vertices, well under UINT32_MAX (2^32 -1).
// I dont use 64 bits everywhere, because the col_idx array has 
// one entry per edge, which means billions of them. At 32 bits it needs
// ~14 GB, at 64 bits it needs ~28 BG. BFS is memory-bandwidth-bound, so 
// halving the bytes moved is close to double the speed.
typedef uint32_t vid_t;

// Edge index. This one must be 64 bits. The largest graph that is targeted
// has ~4.3 billion edges, which means that it does not fit in 32 bits.
// The array is only n+1 (where n the # of vertices), so the memory cost is
// negligible.
typedef uint64_t eid_t;

// printf format specifiers that match the typedefs above.
// Used as: printf("n = %" PRIvid "\n", g->n);
// Writing %u or %lu by hand breaks the moment a typedef changes, and
// the compiler will not always warn.
#define PRIvid PRIu32
#define PRIeid PRIu64

#define VID_MAX UINT32_MAX

/* ------------------------------------------------------------------
 * Compressed Sparse Row storage for an undirected, unweighted graph.
 *
 * The neighbours of vertex v live in col_idx at positions
 *
 *     row_ptr[v]  ...  row_ptr[v + 1] - 1
 *
 * so deg(v) = row_ptr[v+1] - row_ptr[v]. The row_ptr array has n+1
 * entries: the extra one at the end removes the need to special-case
 * the last vertex.
 *
 * Because the graph is undirected, every edge {u,v} appears twice:
 * once as v in u's list and once as u in v's list. Therefore
 * m == 2 * (number of undirected edges).
 * ------------------------------------------------------------------ */
typedef struct {
    vid_t n;        // number of vertices.
    eid_t m;        // number of directed entries.
    eid_t *row_ptr; // length n + 1, non-decreasing, starts at 0.
    vid_t *col_idx; // length m, contains the neighbours of each vertex
} csr_t;

// Degree of a vertex. Marke inline because it sits in the innermost
// loop of every BFS -- a functions call there would be pure overhead.
static inline eid_t csr_degree(const csr_t *g, vid_t v) {
    return g->row_ptr[v + 1] - g->row_ptr[v];
}

/* Half-open range [begin, end) of v's neighbours inside col_idx.
* Typical use:
*
*     for (eid_t e = csr_begin(g, v); e < csr_end(g, v); e++) {
*         vid_t u = g->col_idx[e];
*         ...
*     }
*/
static inline eid_t csr_begin(const csr_t *g, vid_t v) {
    return g->row_ptr[v];
}
static inline eid_t csr_end(const csr_t *g, vid_t v) {
   return g->row_ptr[v + 1];
}

//?????????????????????
void csr_free(csr_t *g);