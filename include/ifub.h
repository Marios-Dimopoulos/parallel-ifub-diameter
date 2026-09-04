#ifndef IFUB_H
#define IFUB_H

#include "csr.h"
#include "bfs_eccentricity.h"

/* Computes the exact diameter of g using the iFUB algorithm
 *
 * 'u' is the startiv vertex -- typically the results of two_sweep().
 * 'lb_init' is an initial lower bound already known -- typically the
 * ecc(a) value two_sweep() also produced. Passing 0 is always safe 
 * if no such bound is available.
 * 
 * 'dist' is caller-allocated scratch space of g->n entries, reused 
 * across every internal BFS call this function makes.
 * 
 * Algorithm:
 *  1. BFS from u gives h = ecc(u), and dist[] now holds, for every 
 * vertex, its distance (its "level") from u.
 *  2. Group every vertex by level: level i holds all v with
 *     dist[v] == i. This partition is fixed for the rest of the run
 *     -- it does not change even though dist[] itself will be 
 *     overwritten by every subsequent BFS call below.
 *  3. lb = max(lb_init, h), ub = 2h.
 *  4. Walk the levels from h down to 1. For every vertex v in the 
 *     current level, run BFS(v) and fold ecc(v) into lb.
 *  5. After finishing a level i, tighten ub = min(ub, 2*(i-1)).
 *     if lb >= ub at any point, the answer is already exact --
 *     stop immediately.
 * 
 * Returns the exact diameter. */
dist_t ifub_diameter(const csr_t *g, vid_t u, dist_t lb_init, dist_t *dist, uint64_t *bfs_count);

#endif