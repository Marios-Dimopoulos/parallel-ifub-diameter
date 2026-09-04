#ifndef BFS_ECCENTRICITY_H
#define BFS_ECCENTRICITY_H

#include <stdint.h>
#include <inttypes.h>
#include "csr.h"

/* Distance from the BFS source. Signed on purpose: i need a value
 * that can never be a real distance, to mark "not reached". -1 reads
 * naturally ("no valid distance") and is easy to test for.*/
typedef int32_t dist_t;
#define DIST_UNREACHED (-1)
#define PRIdist PRId32

/* Runs a single-source BFS from 'source' over 'g'.
 *
 * 'dist' must already be allocated by the caller with g->n entries.
 * On return, dist[v] holds the number of edges on the shortest path
 * from 'source' to 'v', or DIST_UNREACHED if 'v' is not reachable
 * (e.g. it lives in a different connected component). dist[source]
 * is always 0.
 * 
 * Returns the eccentricity of 'source': the largest distance found,
 * i.e. max(dist[v]) over all reached v. This is exactly what the 
 * later 2-sweep / iFUB algorithm needs from every BFS they run, so 
 * i compute it here once rather than making every caller scan 
 * dist[] again afterwards. */
dist_t bfs_eccentricity(const csr_t *g, vid_t source, dist_t *dist);

#endif