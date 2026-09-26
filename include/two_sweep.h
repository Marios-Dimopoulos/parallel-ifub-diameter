#ifndef TWO_SWEEP_H
#define TWO_SWEEP_H

#include "csr.h"
#include "bfs_eccentricity.h"

vid_t two_sweep(const csr_t *g, dist_t *dist, dist_t *lb_out);

#endif