#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h> 
#include <omp.h> 

#include "ifub.h"

/* It's value is depended on the input of the user in the command line.
 * In main.c, that input is parsed and it's value is set accordingly.
 * (ifub_verbose is declared in ifub.h file as a global variabel --
 * it's needed in order for being able to change from main.c file). */
int ifub_verbose = 0;   

/* ifub_diameter -- exact diameter via the iFUB algorithm, with the 
 * inner fringe loop parallelised across threads using OpenMP tasks.
 *
 * 'u': Starting vertex, typically the output of two_sweep().
 * 'lb_init': An already-known lower bound, typically two_sweep()'s
 *            ecc(a) value.
 * 'dist': Caller-allocated of g->n entries, used ONLY for the very 
 *         first, single-threaded BFS below -- every later, parallel
 *         BFS uses its own private slice of dist_pool instead. 
 * 'bfs_count': caller-allocated ooutput. Filled with the exact number
 *              of BFS calls actually performed.*/
dist_t ifub_diameter(const csr_t *g, vid_t u, dist_t lb_init, dist_t *dist, uint64_t *bfs_count) {
    /* Step 1: one ordinary, single-threaded BFS from u.
     *
     * After this call, dist[v] holds the distance from u to every
     * vertex v, and h is the height of that BFS tree -- exactly
     * ecc(u). Since the graph is guaranteed (by problem constraints (and by the upcoming block of code))
     * to be a single connected component, every vertex is reached;
     * there is no DIST_UNREACHED entry anywhere in dist[]. */
    dist_t h = bfs_eccentricity(g, u, dist);
    if (ifub_verbose) {
        fprintf(stderr, "[debug] initial BFS done, h=%" PRIdist "\n", h);
    }

    /* Explicit single-connected-component check, done HERE rather than
     * back in mtx.c, and deliberately not relying on any connectivity
     * metadata a source file might embed in its comments (e.g. SNAP's
     * "% Nodes in largest WCC" lines): such metadata is optimal, only
     * present in some datasets, and is just what the data provider
     * chose to report -- never a guarantee. This check instead asks the 
     * one question that actually matters for correctness: did the BFS
     * i just ran, from this specific graph's own CSR structure, reach 
     * every vertex??  That is the only thing csr_build_from_mtx() cannot 
     * already tell us -- it happily builds a perfectly valid CSR out of a 
     * disconnected graph, since nothing about "how many components
     * exist" is knowable from the file's header alone, before a real 
     * traversal is performed.
     * 
     * I check it right here, immediately after the very first BFS,
     * because that is the earliest point where the answer actually
     * exists: dist[v] == DIST_UNREACHED for any v means the graph has
     * more than one component. Skipping this check would let an 
     * unreachable vertex's dist[v] == -1 be used as an array index a 
     * few lines below (scratch_buffer[dist[v]]), silently corrupting
     * memory before or after the true array bounds -- exactly the 
     * munmap_chunk(): invalid pointer crash this check exists to 
     * prevent, on graphs like SNAP/roadNet-PA that are 99.95% connected 
     * but not fully. */
    for (vid_t v = 0; v < g->n; v++) {
        if (dist[v] == DIST_UNREACHED) {
            fprintf(stderr, "ifub: graph is not a single connected component "
                            "(vertex %" PRIvid " unreachable from source %" PRIvid ")\n", v, u);
            return -1;
        }
    }

    /* Step 2: group every vertex by its level (its distance from u).
     * 
     * This is the EXACT same counting-sort pattern used in
     * csr_build_from_mtx() to build row_ptr/col_idx: count how many
     * vertices fall into each level, turn those counts into 
     * cumulative start offsets via a prefix sum, then place each
     * vertex into its slot.
     * 
     * level_start[i] .. level_start[i+1]-1 will be the range of 
     * positions inside level_verts holding exactly the vertices at
     * distance i from u. calloc() is required (not malloc()) because
     * the counting step below relies on every counter starting at 
     * exactly zero. */
    eid_t *level_start = calloc((size_t)h + 2, sizeof(eid_t));
    if (!level_start) {
        fprintf(stderr, "ifub: out of memory (level_start)\n");
        return -1;
    }

    /* COUNTING pass: level_start[dist[v]+1] counts how many vertices
     * sit at each distance. */
    for (vid_t v = 0; v < g->n; v++) {
        level_start[dist[v] + 1]++;
    }
    /* PREFIX SUM pass: turns raw per-level counts into cumulative
     * start offsets. After this, level_start[i] is exactly the index
     * inside level_verts where level i's vertices begin. */
    for (dist_t lvl = 0; lvl <= h; lvl++) {
       level_start[lvl + 1] += level_start[lvl];
    }
     /* Since every vertex is reachable (single connected component), 
      * level_start[h+1] must be equal to g->n exactly -- every vertex
      * ended up counted into exactly one level. */
    if (ifub_verbose) {
        fprintf(stderr, "[debug] level_start build, total=%" PRIeid "\n", level_start[h + 1]);
    }

    /* level_verts will hold every vertex, REORDERED so that vertices
     * of the same level sit in one contiguous block. scratch_buffer
     * is a disposable copy of level_start, used as a set of "next
     * free slot" cursors during the fill pass below -- level_start
     * itself must stay untouched, since it is the final answer i 
     * for the rest of this function. */
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

    /* FILL pass: for each vertex v look up its level's next free 
     * slot via scratch_buffer[dist[v]], write v there, then advance
     * that specific cursor by one (post-increment). */
    for (vid_t v = 0; v < g->n; v++) {
        level_verts[scratch_buffer[dist[v]]++] = v;
    }
    free(scratch_buffer);

    if (ifub_verbose) {
        fprintf(stderr, "[debug] level_verts built\n");
    }

    /* Thread-local scratch space: a POOL of dist[] arrays, one full 
     * copy per OpenMP thread, allocated ONCE, up front. 
     *
     * Why this is required at all: every task spawned in the fringe
     * loop below calls bfs_eccentricity(), which needs its OWN
     * dist[] array to run correctly (it uses that array both to
     * rerecord distances AND, doubling as a "visited" marker, to know
     * which vertices it has already queued). If two tasks running
     * concurrently on different threads shared a single dist[]
     * array, they would corrupt each other's BFS state -- a classic
     * data race.
     *
     * Giving every THREAD (not every task) its own private slice
     * avoids that: since a single OpenMP thread only ever executes 
     * one task at a time, whichever task currently runs on thread
     * 'tid' can safely use dist_pool's tid-th slice without any
     * locking, because no other thread will ever touch that same
     * slice at the same moment. */
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

    /* Step 3: the fringe loop itself -- the heart of iFUB.
     * 
     * lb starts as the larger of: the lower bound already handed to 
     * me (typically from two_sweep()), or h itself (since 
     * ecc(u) = h is always a valid lower bound no the diameter, by 
     * the triangle-inequality argument: ecc(u) <= D). 
     *
     * ub starts at 2h, the other half of that same triangle
     * inequality: D <= 2*ecc(u). */
    dist_t lb = (lb_init > h) ? lb_init : h;
    dist_t ub = 2 * h;

    /* lb needs to be readable/writable from many threads at once, so
     * it lives in an _Atomic variable instead of a plain dist_t. */
    _Atomic dist_t lb_atomic;
    atomic_init(&lb_atomic, lb);

    /* stop_flag is the cooperative early-termination signal for the 
     * CURRENT level only. */
    _Atomic int stop_flag;
    atomic_init(&stop_flag, 0);

    /* atomic_bfs_counter is a simple atomic counter, incremented once 
     * per BFS actually executed. It exists 
     * purely for the debug progress messages below and for the 
     * final, precise bfs_count the caller receives. */
    _Atomic uint64_t atomic_bfs_counter;
    atomic_init(&atomic_bfs_counter, 0);

    /* Walk fringe levels from the FARTHEST (h) down towards 1.
     * Starting from the farthest level is a deliberate heuristic:
     * vertices already far from u are statistically more likely to 
     * themselves have large eccentricity, so lb tends to rise
     * quickly, letting the lb >= ub termination fire as early as possible. */
    dist_t level = h;
    while (level >= 1 && lb < ub) {
        /* [begin, end) is the half-open range of positions inside
         * level_verts holding exactly this level's vertices -- read
         * directly from the level_start array build in Step 2. */
        eid_t begin = level_start[level];
        eid_t end = level_start[level + 1];

        if (ifub_verbose) {
            fprintf(stderr, "[debug] level=%" PRIdist " begin=%" PRIeid " end=%" PRIeid " lb=%" PRIdist " ub=%" PRIdist "\n",
                    level, begin, end, lb, ub);
        }

        /* omp parallel: spawns the thread team.
         * Every thread reaches the omp single block below, but only 
         * ONE of them actually executes its body -- the rest wait.
         * ready to steal tasks as soon as any appear in the queue. */
        #pragma omp parallel
        {
            #pragma omp single
            {
                /* This for-loop itself runs on exactly ONE thread
                 * (whichever one the runtime picked for 'single').
                 * It does NOT execute any BFS work directly -- each 
                 * iteration merely REGISTERS one task descriptor
                 * and immediateley moves on to create
                 * the next one. The other idle threads from the 
                 * 'parallel' team pick these tasks up concurrently, 
                 * often well before this loop itself has finished
                 * creating all of them. */
                for (eid_t k = begin; k < end; k++) {
                    vid_t v = level_verts[k];

                    /* firstprivate(v): each task gets its OWN private
                     * copy of v's value, taken at the moment the task
                     * was created.
                     * 
                     * shared(...): these variables are the SAME single
                     * instance for every task -- intentional, since 
                     * they represent state that genuinely must be
                     * visible and mutable by all tasks at once (the
                     * graph itself, the per-thread scratch pool, the
                     * shared lower bound, the stop flag, and the 
                     * current level's upper bound). */
                    #pragma omp task firstprivate(v) shared(g, dist_pool, lb_atomic, stop_flag, ub, atomic_bfs_counter)
                    {
                        /* Cooperative cancellation check: if some
                         * OTHER task already proved lb >= ub earlier
                         * in this same level, don't bother starting a 
                         * fresh, now-pointless BFS. Tasks that had 
                         * already started before the flag was raised
                         * are not interrupted -- they simply finish
                         * their one BFS normally; this only prevents 
                         * NEW BFS calls from beginning. */
                        if (!atomic_load(&stop_flag)) {
                            /* Each task looks up ITS OWN slice of
                             * dist_pool, based on which thread happens
                             * to be running it right now. Two tasks 
                             * running concurrently on two different
                             * threads always get two different,
                             * not-overlapping slices -- no locking
                             * needed to use my_dist safely. */
                            int tid = omp_get_thread_num();
                            dist_t *my_dist = dist_pool + (size_t)tid * g->n;

                            /* The actual work: one full BFS from v,
                             * using this thread's private scratch
                             * array. Returns v's eccentricity. */
                            dist_t e = bfs_eccentricity(g, v, my_dist);

                            uint64_t c = atomic_fetch_add(&atomic_bfs_counter, 1) + 1;
                            if (ifub_verbose) {
                                /* Record that one more BFS has genuinely
                                 * completed, and print a progress line
                                 * every 200 cimpletions -- purely a 
                                 * debugging feature, has no effects on 
                                 * the algorith's correctness. */
                                if (c % 200 == 0) {
                                    fprintf(stderr, "  [progress] bfs_count=%" PRIu64
                                            " level=%" PRIdist " lb=%" PRIdist " ub=%" PRIdist "\n",
                                            c, level, atomic_load(&lb_atomic), ub);
                                }   
                            }

                            /* Lock-free "update" lb if bigger" pattern.
                             * Read the current shared lb into old_lb.
                             * If e beats it, attempt to atomically
                             * swap old_lb -> e. If some OTHER task
                             * updated lb_atomic in the meantime, the 
                             * compare-exchange fails, automatically
                             * refreshes old_lb to the new real value,
                             * and the loop retries against that fresh 
                             * value -- guaranteeing no update is ever
                             * silently lost, regardless of how many 
                             * threads race here simoultaneously. */
                            dist_t old_lb = atomic_load(&lb_atomic);
                            while (e > old_lb && !atomic_compare_exchange_weak(&lb_atomic, &old_lb, e));
                            /* empty loop body: all the work 
                             * happens inside the condition 
                             * itself, on every retry. */

                            /* If the bound just became tight enough
                             * to prove the diameter mathematically,
                             * signal every OTHER not-yet-started task
                             * in this level to skip its own BFS. */
                            if (atomic_load(&lb_atomic) >= ub) {
                                atomic_store(&stop_flag, 1);
                            }
                        }
                    }
                }
                /* Block here until every task spawned above (by this
                 * thread) has fully finished running -- on this
                 * thread or any other. Nothing below this point runs
                 * until the ENTIRE level is done. */
                #pragma omp taskwait
            }
        }   /* end parallel: implicit barrier here too, team disbands. */

        /* Pull the final, settled value of lb out of the atomic
         * variable into the ordinary local 'lb', now that all of 
         * this level's tasks are guaranteed to have finished. */
        lb = atomic_load(&lb_atomic);

        /* Tighten the upper bound not that i've fully examined this
         * level: any vertex NOT yet examined lives at distance < level
         * from u, so by the triangle inequality its eccentricity can
         * be at most 2*(level-1). */
        dist_t new_ub = 2 * (level - 1);
        if (new_ub < ub) ub = new_ub;

        level--;    // Move to the next level down.
    }

    /* Report the EXACT number of BFS calls genuinely executed. */
    *bfs_count = atomic_load(&atomic_bfs_counter);

    free(dist_pool);
    free(level_start);
    free(level_verts);
    return lb;  // By this point lb == ub == the exact diameter, or the while-loop ran out of levels entirely.
     
}