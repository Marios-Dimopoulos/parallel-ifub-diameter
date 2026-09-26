/* two_sweep.c -- * Picks a good starting vertex for iFUB using a double sweep:
 * 
 *  1. BFS from an arbitrary vertex r -> farthest vertex a
 *  2. BFS from a -> farthest vertex b
 *
 * ecc(a) (the eccentricity found in step 2) is already a strong
 * lower bound on the diameter -- in practice often exact or very
 * close.
 *
 * The returned starting vertex u is NOT b but the midpoint of a
 * shortest a-b path (at distance ecc(a)/2 from a). iFUB must examine
 * every vertex farther that ~D/2 from u, so a CENTRAL u (ecc(u) ~ D/2)
 * needs far fewer BFS calls to converge than a peripheral one (ecc(u) ~ D).
 * Correctness of iFUB holds for any u; only the speed changes.
 *
 * 'dist' is caller-allocated scratch space of g->n entries, reused
 * across both internal BFS calls. 'lb_out' receives the lower bound described above.
 * 
 * Returns the chosen starting vertex (the midpoint). */

#include "two_sweep.h"

vid_t two_sweep(const csr_t *g, dist_t *dist, dist_t *lb_out) {
    /* Step 1: arbitrary startnig point. Vertex 0 is fine -- any
     * choice works. */
    vid_t r = 0;

    /* Step 1: BFS from an arbitrary vertex r. */
    dist_t ecc_r = bfs_eccentricity(g, r, dist);

    /* The target value (ecc_r) is already known, so i just
     * look for the first vertex that attains it -- guaranteed
     * to exist, since ecc_r is by definition the max value inside dist[]. */
    vid_t a = r;
    for (vid_t v = 0; v < g->n; v++) {
        if (dist[v] == ecc_r) {
            a = v;
            break;
        }
    }

    /* Step 2: BFS from a. Its eccentricity is our lower bound
     * for the iFUB algorithm that will be run after. */
    dist_t ecc_a = bfs_eccentricity(g, a, dist);

    vid_t b = a;
    for (vid_t v = 0; v < g->n; v++) {
        if (dist[v] == ecc_a) {
            b = v;
            break;
        }
    }

    *lb_out = ecc_a;

   /* Walk from b back towards a along a shortest a-b path. dist[]
    * still holds distances from a (the last BFS above), so every
    * vertex with dist = d > 0 has at least one neighbour with
    * dist = d - 1. Stop when the halfway point is reached. */
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

    /* TEMPORARY: reverted to the peripheral vertex b for the
     * peripheral-vs-midpoint comparison run. Restore the midpoint
     * walk below once the peripheral-start results are collected. */
    //return b;

}