/* ifub.c -- exact graph diameter with the iFUB algorithm, parallelised with
 * OpenMP tasks.
 *
 * One BFS from the start vertex u groups the vertices into levels by their
 * distance from u. The levels are then examined from the farthest one down:
 * every vertex of a level gets its own BFS (one OpenMP task each, with a
 * private dist[] per thread). The lower bound lb grows with the
 * eccentricities found, the upper bound ub shrinks with every finished
 * level, and the loop stops as soon as lb >= ub. The algorithm and the
 * parameters of ifub_diameter() are summarised in ifub.h. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <omp.h>

#include "ifub.h"

/* Set by main() from the -v flag (declared in ifub.h). */
int ifub_verbose = 0;

dist_t ifub_diameter(const csr_t *g, vid_t u, dist_t lb_init, dist_t *dist, uint64_t *bfs_count) {
    /* Step 1: one sequential BFS from u. Afterwards dist[v] is the distance
     * from u and h = ecc(u), the height of the BFS tree. */
    dist_t h = bfs_eccentricity(g, u, dist);
    if (ifub_verbose) {
        fprintf(stderr, "[debug] initial BFS done, h=%" PRIdist "\n", h);
    }

    /* Connectivity check. It is done here and not in mtx.c/csr.c because the
     * CSR builder happily builds a valid CSR from a disconnected graph, and
     * metadata in the file's comments (e.g. SNAP's largest-component counts)
     * is no guarantee. The first BFS is the earliest point where the answer
     * exists: any dist[v] == DIST_UNREACHED means several components. It must
     * come before the level grouping below, where dist[v] is used as an array
     * index (a -1 would write out of bounds -- this once crashed with
     * "munmap_chunk(): invalid pointer" on a nearly, but not fully, connected
     * graph). */
    for (vid_t v = 0; v < g->n; v++) {
        if (dist[v] == DIST_UNREACHED) {
            fprintf(stderr, "ifub: graph is not a single connected component "
                            "(vertex %" PRIvid " unreachable from source %" PRIvid ")\n", v, u);
            return -1;
        }
    }

    /* Step 2: group the vertices by level (distance from u) with a counting
     * sort, the same pattern as in csr_build_from_mtx(): count the vertices
     * per level, prefix-sum the counts into start offsets, then place each
     * vertex. Level i occupies level_verts[level_start[i] ..
     * level_start[i+1]-1]. calloc: the counters must start at zero. */
    eid_t *level_start = calloc((size_t)h + 2, sizeof(eid_t));
    if (!level_start) {
        fprintf(stderr, "ifub: out of memory (level_start)\n");
        return -1;
    }

    /* COUNTING pass: level_start[dist[v]+1] counts the vertices at each
     * distance. */
    for (vid_t v = 0; v < g->n; v++) {
        level_start[dist[v] + 1]++;
    }
    /* PREFIX SUM pass: after this, level_start[i] is the index inside
     * level_verts where level i begins, and level_start[h+1] == g->n. */
    for (dist_t lvl = 0; lvl <= h; lvl++) {
       level_start[lvl + 1] += level_start[lvl];
    }
    if (ifub_verbose) {
        fprintf(stderr, "[debug] level_start build, total=%" PRIeid "\n", level_start[h + 1]);
    }

    /* level_verts holds every vertex, reordered so that the vertices of one
     * level are contiguous. scratch_buffer is a disposable copy of
     * level_start used as "next free slot" cursors, because level_start
     * itself must stay intact. */
    vid_t *level_verts = malloc((size_t)g->n * sizeof(vid_t));
    eid_t *scratch_buffer = malloc(((size_t)h + 2) * sizeof(eid_t));
    if (!level_verts || !scratch_buffer) {
        fprintf(stderr, "ifub: out of memory (level arrays)\n");
        free(level_start);
        free(level_verts);
        free(scratch_buffer);
        return -1;
    }
    memcpy(scratch_buffer, level_start, ((size_t)h + 2) * sizeof(eid_t));

    /* FILL pass: write each vertex into the next free slot of its level. */
    for (vid_t v = 0; v < g->n; v++) {
        level_verts[scratch_buffer[dist[v]]++] = v;
    }
    free(scratch_buffer);

    if (ifub_verbose) {
        fprintf(stderr, "[debug] level_verts built\n");
    }

    /* One private dist[] per OpenMP thread, allocated once. Every task calls
     * bfs_eccentricity(), which uses its dist[] both for the distances and
     * as the visited marker, so two concurrent BFS calls must not share one
     * (data race). The slice is per THREAD, not per task: a thread runs one
     * task at a time, so slice 'tid' is never used by two tasks at once, with
     * no locking. */
    int max_threads = omp_get_max_threads();
    dist_t *dist_pool = malloc((size_t)max_threads * g->n * sizeof(dist_t));
    if (!dist_pool) {
        fprintf(stderr, "ifub: out of memory (dist_pool)\n");
        free(level_start);
        free(level_verts);
        return -1;
    }
    if (ifub_verbose) {
        fprintf(stderr, "[debug] dist_pool allocated, %d threads x %" PRIvid " vertices\n", max_threads, g->n);
    }

    /* Step 3: the fringe loop -- the heart of iFUB.
     *
     * lb = max(lb_init, h), since ecc(u) <= D. ub = 2h, since
     * D <= 2*ecc(u) (triangle inequality through u). */
    dist_t lb = (lb_init > h) ? lb_init : h;
    dist_t ub = 2 * h;

    /* lb is read and written by many threads at once, so it is atomic. */
    _Atomic dist_t lb_atomic;
    atomic_init(&lb_atomic, lb);

    /* Cooperative early-stop signal: raised when a task proves lb >= ub. It
     * is never reset because the level loop below ends right after that
     * level (lb never decreases and ub never increases). */
    _Atomic int stop_flag;
    atomic_init(&stop_flag, 0);

    /* Number of BFS calls actually executed; it becomes *bfs_count. */
    _Atomic uint64_t atomic_bfs_counter;
    atomic_init(&atomic_bfs_counter, 0);

    /* Walk the levels from the farthest (h) down to 1. This order is
     * required by the bound (ub = 2*(i-1) is valid only once ALL levels
     * above i were examined), and vertices far from u tend to have a large
     * eccentricity, so lb rises quickly. */
    dist_t level = h;
    while (level >= 1 && lb < ub) {
        /* [begin, end) is this level's range inside level_verts. */
        eid_t begin = level_start[level];
        eid_t end = level_start[level + 1];

        if (ifub_verbose) {
            fprintf(stderr, "[debug] level=%" PRIdist " begin=%" PRIeid " end=%" PRIeid " lb=%" PRIdist " ub=%" PRIdist "\n",
                    level, begin, end, lb, ub);
        }

        /* One parallel region per level: a single thread only CREATES the
         * tasks (one per vertex of the level, no BFS work of its own) and
         * the whole team executes them, often before the creation loop
         * has even finished. */
        #pragma omp parallel
        {
            #pragma omp single
            {
                for (eid_t k = begin; k < end; k++) {
                    vid_t v = level_verts[k];

                    /* firstprivate(v): each task keeps its own copy of v.
                     * shared(...): state that all tasks must see (graph,
                     * scratch pool, lb, stop flag, this level's ub). */
                    #pragma omp task firstprivate(v) shared(g, dist_pool, lb_atomic, stop_flag, ub, atomic_bfs_counter)
                    {
                        /* Skip the BFS if another task already proved
                         * lb >= ub in this level. Tasks that already
                         * started finish their BFS normally. */
                        if (!atomic_load(&stop_flag)) {
                            /* The private slice of dist_pool of the thread
                             * running this task. */
                            int tid = omp_get_thread_num();
                            dist_t *my_dist = dist_pool + (size_t)tid * g->n;

                            /* The actual work: one BFS from v. */
                            dist_t e = bfs_eccentricity(g, v, my_dist);

                            uint64_t c = atomic_fetch_add(&atomic_bfs_counter, 1) + 1;
                            if (ifub_verbose) {
                                /* Progress line every 200 BFS calls
                                 * (diagnostics only). */
                                if (c % 200 == 0) {
                                    fprintf(stderr, "  [progress] bfs_count=%" PRIu64
                                            " level=%" PRIdist " lb=%" PRIdist " ub=%" PRIdist "\n",
                                            c, level, atomic_load(&lb_atomic), ub);
                                }
                            }

                            /* Lock-free "lb = max(lb, e)". If another
                             * thread changes lb_atomic in the meantime, the
                             * compare-exchange fails and refreshes old_lb,
                             * and the loop retries, so no update is lost.
                             * The loop body is empty: all the work is in the
                             * condition. */
                            dist_t old_lb = atomic_load(&lb_atomic);
                            while (e > old_lb && !atomic_compare_exchange_weak(&lb_atomic, &old_lb, e));

                            /* lb >= ub proves the diameter: tell the
                             * not-yet-started tasks of this level to skip
                             * their BFS. */
                            if (atomic_load(&lb_atomic) >= ub) {
                                atomic_store(&stop_flag, 1);
                            }
                        }
                    }
                }
                /* Wait for every task of this level: the bounds are only
                 * valid once all its vertices have been examined. */
                #pragma omp taskwait
            }
        }   /* end parallel: implicit barrier, the team disbands. */

        /* All tasks of the level are done: read the settled lb. */
        lb = atomic_load(&lb_atomic);

        /* Tighten ub. Any pair of unexamined vertices lies at distance
         * < level from u, so through u they are at most 2*(level-1) apart
         * (pairs that include an examined vertex are covered by lb). */
        dist_t new_ub = 2 * (level - 1);
        if (new_ub < ub) ub = new_ub;

        level--;    // Move to the next level down.
    }

    /* The number of BFS calls actually executed. */
    *bfs_count = atomic_load(&atomic_bfs_counter);

    free(dist_pool);
    free(level_start);
    free(level_verts);
    return lb;  // The loop ended with lb >= ub (or all levels were examined): lb is the exact diameter.

}
