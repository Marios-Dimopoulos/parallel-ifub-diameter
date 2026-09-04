#ifndef TWO_SWEEP_H
#define TWO_SWEEP_H

#include "csr.h"
#include "bfs_eccentricity.h"

/* Picks a good starting vertex for iFUB using a double sweep:
 * 
 *  1. BFS from an arbitrary vertex r -> farthest vertex a
 *  2. BFS from a -> farthes vertex b
 * 
 * ecc(a) (the accentricity found in step 2) is already a strong 
 * lower bound on the diameter -- in practice often exact or very 
 * close. b is returned as the recommended starting vertex u for
 * iFUB: peripheral vertices like b tend to give a compact BFS tree,
 * which means fewer fringe levels for iFUB to examine later.
 *
 * 'dist' is caller-allocated scratch space of g->n entries, reused 
 * across both internal BFS calls -- avoids two separate allocations
 * 'lb_out' receives the lower bound described above.
 * 
 * Returns the chosen starting vertex b. */

vid_t two_sweep(const csr_t *g, dist_t *dist, dist_t *lb_out);

#endif