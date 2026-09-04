#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "ifub.h"

dist_t ifub_diameter(const csr_t *g, vid_t u, dist_t lb_init, dist_t *dist, uint64_t *bfs_count) {
    /* Step 1: BFS from u. After this call, dist[v] is the distance
     * from u to v for every v, and h is the height of that BFS tree
     * -- exactly ecc(u). Since the graph is guaranteed to be a single
     * connected component, every vertex is reached. */
    dist_t h = bfs_eccentricity(g, u, dist);

    /* Step 2: group every vertex by its level (distance from u),
     * using the exact same counting-sort pattern as csr_build_from_mtx:
     * count how many vertices fall in each level, prefix-sum those
     * counts into start offsets, then place each vertex into its slot.
     * 
     * level_start[i] .. level_start[i+1]-1 are the positions inside 
     * level_verts holding exactly the vertices at distance i. */
    eid_t *level_start = calloc((size_t)h + 2, sizeof(eid_t));
    if (!level_start) {
        fprintf(stderr, "ifub: out of memory (level_start)\n");
        return -1;
    }

    for (vid_t v = 0; v < g->n; v++) {
        level_start[dist[v] + 1]++;
    }
    for (dist_t lvl = 0; lvl <= h; lvl++) {
        level_start[lvl + 1] += level_start[lvl];
    }
    /* level_start[h+1] now equals g->n exactly, since every vertex 
     * is reachable -- no DIST_UNREACHED entries to worry about. */

    vid_t *level_verts = malloc((size_t)g->n * sizeof(vid_t));
    eid_t *cursor = malloc(((size_t)h + 2) * sizeof(eid_t));
    if (!level_verts || !cursor) {
        fprintf(stderr, "ifub: out of memory (level arrays)\n");
        free(level_start);
        free(level_verts);
        free(cursor);
        return -1;
    }
    memcpy(cursor, level_start, ((size_t)h + 2)*sizeof(eid_t));

    for (vid_t v = 0; v < g->n; v++) {
        level_verts[cursor[dist[v]]++] = v;
    }
    free(cursor);

    /* Step 3: the fringe loop. Walk levels from h down to 1.
     *
     * dist[] gets completely overwritten by each bfs_eccentricity()
     * call inside this loop -- that is fine, because level_verts
     * already holds a frozen snapshot of which vertex belongs to 
     * which level, computed once before this loop starts. */
    dist_t lb = (lb_init > h) ? lb_init : h;
    dist_t ub = 2 * h;

    dist_t level = h;
    while (level >= 1 && lb < ub) {
        eid_t begin = level_start[level];
        eid_t end = level_start[level + 1];
        
        for (eid_t k = begin; k < end; k++) {
            vid_t v = level_verts[k];
            dist_t e = bfs_eccentricity(g, v, dist);
            (*bfs_count)++;
            if (e > lb) {
                lb = e;
            }

            /* No point running the remaining BFS in this level once
             * the bound is already tight enough. */
             if (lb >= ub) {
                break;
             }
        }

        dist_t new_ub = 2 * (level - 1);
        if (new_ub < ub) {
            ub = new_ub;
        }
        
        level--;
    }

    free(level_start);
    free(level_verts);
    return lb;
}