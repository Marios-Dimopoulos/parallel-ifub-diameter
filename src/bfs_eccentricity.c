/* bfs_eccentricity.c -- single-source BFS over the CSR graph.
 *
 * Fills dist[] (caller-allocated, g->n entries) with the distance from
 * 'source' to every vertex (DIST_UNREACHED for vertices in other components)
 * and returns the eccentricity of 'source': the largest distance found.
 * Returns -1 if the internal queue cannot be allocated. dist[] doubles as
 * the visited marker and the function keeps no global state, so it can run
 * concurrently from several threads as long as each passes its own dist[]. */

#include <stdlib.h>
#include <stdio.h>
#include "bfs_eccentricity.h"

dist_t bfs_eccentricity(const csr_t *g, vid_t source, dist_t *dist) {
    /* Every vertex starts "not reached". dist[v] == DIST_UNREACHED means
     * "never queued", anything else means "already queued". */
    for (vid_t v = 0; v < g->n; v++) {
        dist[v] = DIST_UNREACHED;
    }

    /* Each vertex is enqueued at most once, so a plain array of size n with
     * two moving indices is enough -- no circular buffer needed. */
    vid_t *queue = malloc((size_t)g->n * sizeof(vid_t));
    if (!queue) {
        fprintf(stderr, "bfs: out of memory (queue, %" PRIvid " vertices)\n", g->n);
        return -1;
    }

    vid_t head = 0, tail = 0;

    dist[source] = 0;
    queue[tail++] = source;

    dist_t eccentricity = 0;    // Largest distance seen so far.

    /* Standard BFS: pull a vertex off the front, push its unvisited
     * neighbours onto the back. Every vertex is pushed exactly once. */
    while (head < tail) {
        vid_t u = queue[head++];
        dist_t du = dist[u];

        for (eid_t e = csr_begin(g, u); e < csr_end(g, u); e++) {
            vid_t v = g->col_idx[e];

            if (dist[v] == DIST_UNREACHED) {
                dist[v] = du + 1;
                if (dist[v] > eccentricity) {
                    eccentricity = dist[v];
                }
                queue[tail++] = v;
            }
        }

    }

    free(queue);
    return eccentricity;
}
