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

dist_t bfs_eccentricity(const csr_t *g, vid_t source, dist_t *dist);

#endif