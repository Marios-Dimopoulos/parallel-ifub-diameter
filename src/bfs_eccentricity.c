/* bfs_eccentricity.c -- Runs a single-source BFS from 'source' over 'g'.
 *
 * 'dist' must already be allocated by the caller with g->n entries.
 * On return, dist[v] holds the number of edges on the shortest path
 * from 'source' to 'v', or DIST_UNREACHED if 'v' is not reachable. 
 * dist[source] is always 0.
 * 
 * Returns the eccentricity of 'source': the largest distance found,
 * over all reached v. This is exactly what the 
 * later 2-sweep / iFUB algorithm needs from every BFS they run. */

#include <stdlib.h>
#include <stdio.h>
#include "bfs_eccentricity.h"

dist_t bfs_eccentricity(const csr_t *g, vid_t source, dist_t *dist) {
    /* Every vertex starts "not reached". I mark them all up front
     * rather than trying to track "seen vs unseen" seprately --
     * dist[] itself doubles as the visited marker: dist[v] ==
     * DIST_UNREACHED means "never queued", anything else means
     * "already queued, do not queue again". */
    for (vid_t v = 0; v < g->n; v++) {
        dist[v] = DIST_UNREACHED;
    }

    /* Each vertex is enqueued at most once in a BFS, so a 
     * plain array of size n with two moving indices is enough --
     * no need for a circular buffer. */
    vid_t *queue = malloc((size_t)g->n * sizeof(vid_t));
    if (!queue) {
        fprintf(stderr, "bfs: out of memory (queue, %" PRIvid " vertices)\n", g->n);
        return -1;
    }

    vid_t head = 0, tail = 0;

    dist[source] = 0;
    queue[tail++] = source;

    dist_t eccentricity = 0;    // Largest distance seen so far.

    /* Standard BFS: pull a vertex off the front, look at its 
     * neighbours, push the unvisited ones onto the back. Because
     * every edge is examined from the vertex that discovers it, and 
     * every vertex is pushed exactly once. */
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