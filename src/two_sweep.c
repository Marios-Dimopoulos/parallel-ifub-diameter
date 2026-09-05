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
    return b;
}