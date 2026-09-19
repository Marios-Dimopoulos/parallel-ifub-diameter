#ifndef TWO_SWEEP_H
#define TWO_SWEEP_H

#include "csr.h"
#include "bfs_eccentricity.h"

/* Picks a good starting vertex for iFUB using a double sweep:
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
 * across both internal BFS calls -- avoids two separate allocations
 * 'lb_out' receives the lower bound described above.
 * 
 * Returns the chosen starting vertex (the midpoint). */
vid_t two_sweep(const csr_t *g, dist_t *dist, dist_t *lb_out);

#endif