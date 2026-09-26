#ifndef IFUB_H
#define IFUB_H

#include "csr.h"
#include "bfs_eccentricity.h"

/* When non-zero, ifub_diameter() prints its internal [debug]/[progress]
 * diagnostics to stderr. Off by default -- set by the caller (from the -v
 * command-line flag) before calling ifub_diameter(). */
extern int ifub_verbose;

/* Computes the exact diameter of g using the iFUB algorithm.
 *
 * 'u' is the starting vertex -- typically the result of two_sweep().
 * 'lb_init' is an initial lower bound already known -- typically the
 * ecc(a) value two_sweep() also produced.
 * 'dist' is caller-allocated (g->n entries) and used only for the first,
 * single-threaded BFS; the parallel BFS calls use their own per-thread
 * arrays.
 * 'bfs_count' is an output: the number of BFS calls the fringe loop
 * actually performed (it does not include the two BFS of two_sweep() nor
 * the first BFS from u).
 *
 * Algorithm:
 *  1. BFS from u gives h = ecc(u), and dist[] now holds, for every
 *     vertex, its distance (its "level") from u.
 *  2. Group every vertex by level: level i holds all v with
 *     dist[v] == i. This partition is fixed for the rest of the run.
 *  3. lb = max(lb_init, h), ub = 2h.
 *  4. Walk the levels from h down to 1. For every vertex v in the
 *     current level, run BFS(v) and fold ecc(v) into lb.
 *  5. After finishing a level i, tighten ub = min(ub, 2*(i-1)).
 *     If lb >= ub at any point, the answer is already exact --
 *     stop immediately.
 *
 * Returns the exact diameter, or -1 on failure (the graph is not a single
 * connected component, or out of memory). */
dist_t ifub_diameter(const csr_t *g, vid_t u, dist_t lb_init, dist_t *dist, uint64_t *bfs_count);

#endif
