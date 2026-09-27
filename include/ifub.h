#ifndef IFUB_H
#define IFUB_H

#include "csr.h"
#include "bfs_eccentricity.h"

/* When non-zero, ifub_diameter() prints its internal [debug]/[progress]
 * diagnostics to stderr. Off by default -- set by the caller (from the -v
 * command-line flag) before calling ifub_diameter(). */
extern int ifub_verbose;

dist_t ifub_diameter(const csr_t *g, vid_t u, dist_t lb_init, dist_t *dist, uint64_t *bfs_count);

#endif
