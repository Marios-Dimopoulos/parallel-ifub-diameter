/* two_sweep.c -- picks iFUB's starting vertex and an initial lower bound
 * (double sweep).
 *
 *  1. BFS from vertex 0 -> farthest vertex a
 *  2. BFS from a -> farthest vertex b; ecc(a) is a lower bound on the
 *     diameter (often exact or very close)
 *
 * The returned start vertex is the midpoint of a shortest a-b path (at
 * distance ecc(a)/2 from a), not b: iFUB must examine every vertex farther
 * than ~D/2 from its start vertex, so a central one (ecc(u) ~ D/2) needs far
 * fewer BFS calls than a peripheral one (ecc(u) ~ D). Correctness of iFUB
 * holds for any start vertex; only the speed changes.
 *
 * 'dist' is caller-allocated scratch space (g->n entries) reused by both BFS
 * calls; 'lb_out' receives the lower bound. */

#include "two_sweep.h"

vid_t two_sweep(const csr_t *g, dist_t *dist, dist_t *lb_out) {
    /* Step 1: BFS from an arbitrary vertex r (vertex 0 is fine, any choice
     * works). */
    vid_t r = 0;
    dist_t ecc_r = bfs_eccentricity(g, r, dist);

    /* The first vertex that attains ecc_r is the farthest vertex a (it
     * exists by definition of the eccentricity). */
    vid_t a = r;
    for (vid_t v = 0; v < g->n; v++) {
        if (dist[v] == ecc_r) {
            a = v;
            break;
        }
    }

    /* Step 2: BFS from a. Its eccentricity is the lower bound for iFUB. */
    dist_t ecc_a = bfs_eccentricity(g, a, dist);

    vid_t b = a;
    for (vid_t v = 0; v < g->n; v++) {
        if (dist[v] == ecc_a) {
            b = v;
            break;
        }
    }

    *lb_out = ecc_a;

    /* Walk from b back towards a along a shortest a-b path. dist[] still
     * holds distances from a (the last BFS above), so every vertex with
     * dist = d > 0 has at least one neighbour with dist = d - 1. Stop when
     * the halfway point is reached. */
    vid_t cur = b;
    while (dist[cur] > ecc_a / 2) {
        for (eid_t e = csr_begin(g, cur); e < csr_end(g, cur); e++) {
            vid_t w = g->col_idx[e];
            if (dist[w] == dist[cur] - 1) {
                cur = w;
                break;
            }
        }
    }
    return cur;
}
