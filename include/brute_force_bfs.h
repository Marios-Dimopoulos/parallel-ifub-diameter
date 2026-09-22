#ifndef BRUTE_FORCE_BFS_H
#define BRUTE_FORCE_BFS_H

#include "csr.h"
#include "bfs_eccentricity.h"

/* Exact diameter by brute force: one BFS from EVERY vertex, diameter =
 * max eccentricity. It exists only to fact-check the iFUB result, so it
 * is deliberately simple and shares no logic with ifub.c except the
 * single-source BFS (bfs_eccentricity).
 *
 * The BFS calls are independent, so they are spread over the OpenMP
 * threads; each thread owns one private dist[] array.
 *
 * Requires a single connected component: the graph is checked with one
 * BFS from vertex 0 first, and -1 is returned if some vertex is unreachable.
 *
 * Returns the diameter. If 'show_progress' is non-zero, prints a
 * progress line to stderr every 5% of the BFS calls.
 *
 * Returns -1 on failure (not connected, out of memory). */
dist_t brute_force_diameter(const csr_t *g, int show_progress);

#endif