/* brute_force_bfs.c -- independent check of the iFUB result.
 *
 * Reads a .mtx graph exactly like the main program (mtx.c + csr.c),
 * runs one BFS from every vertex (bfs_eccentricity.c) and reports the
 * exact diameter. Optionally compares it with an expected value.
 *
 * Usage: ./brute_force_bfs <file.mtx> [expected_diameter]
 *
 * Exit code: 0 = done (and matches, if an expected value was given),
 *            1 = error, 2 = the diameter differs from the expected one. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <omp.h>

#include "mtx.h"
#include "csr.h"
#include "bfs_eccentricity.h"
#include "brute_force_bfs.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

dist_t brute_force_diameter(const csr_t *g, int show_progress) {
    const int64_t n = (int64_t)g->n;

    /* Connectivity check: one BFS from vertex 0 must reach every vertex. */
    dist_t *check = malloc((size_t)g->n * sizeof(dist_t));
    if (!check) {
        fprintf(stderr, "brute force: out of memory (check array)\n");
        return -1;
    }
    if (bfs_eccentricity(g, 0, check) < 0) {
        free(check);
        return -1;
    }
    for (int64_t v = 0; v < n; v++) {
        if (check[v] == DIST_UNREACHED) {
            fprintf(stderr, "brute force: graph is not connected (vertex %" PRId64
                            " unreachable from vertex 0)\n", v);
            free(check);
            return -1;
        }
    }
    free(check);

    dist_t diameter = -1;

    _Atomic uint64_t done;
    atomic_init(&done, 0);
    _Atomic int failed;
    atomic_init(&failed, 0);
    const uint64_t step = (uint64_t)(n / 20 > 0 ? n / 20 : 1);

    #pragma omp parallel
    {
        dist_t *dist = malloc((size_t)g->n * sizeof(dist_t));
        if (!dist) {
            fprintf(stderr, "brute force: out of memory (thread dist array)\n");
            atomic_store(&failed, 1);
        }

        dist_t local_diam = -1;

        /* Every thread must reach the worksharing loop, even after a
         * failure, so the body just skips the work. */
        #pragma omp for schedule(dynamic, 16)
        {   for (int64_t s = 0; s < n; s++) {
                // I cannot use "break" to exit the loop on failure, because that would leave some thread behind.
                if (!dist || atomic_load(&failed)) {
                    continue;
                }

                dist_t e = bfs_eccentricity(g, (vid_t)s, dist);
                if (e < 0) {
                    atomic_store(&failed, 1);
                    continue;
                }
                if (e > local_diam) {
                    local_diam = e;
                }

                uint64_t c = atomic_fetch_add(&done, 1) + 1;
                if (show_progress && c % step == 0) {
                    fprintf(stderr, "  [progress] %" PRIu64 " / %" PRId64 " BFS done\n", c, n);
                }
            }
        }
        #pragma omp critical
        {
            if (local_diam > diameter) {
                diameter = local_diam;
            }
        }
        free(dist);
    }

    if (atomic_load(&failed)) {
        return -1;
    }
    return diameter;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <file.mtx> [expected_diameter]\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    int have_expected = (argc == 3);
    long expected = have_expected ? strtol(argv[2], NULL, 10) : 0;

    mtx_t mx;
    if (mtx_open(path, &mx) != 0) {
        fprintf(stderr, "failed to open %s\n", path);
        return 1;
    }
    csr_t g;
    if (csr_build_from_mtx(&mx, &g) != 0) {
        fprintf(stderr, "failed to build CSR from %s\n", path);
        mtx_close(&mx);
        return 1;
    }
    mtx_close(&mx);

    double t0 = now();
    dist_t diameter = brute_force_diameter(&g, 1);
    double t1 = now();

    if (diameter < 0) {
        fprintf(stderr, "brute force failed for %s\n", path);
        csr_free(&g);
        return 1;
    }

    printf("graph                  : %s\n", path);
    printf("threads                : %d\n", omp_get_max_threads());
    printf("vertices               : %" PRIvid "\n", g.n);
    printf("edges(directed entries): %" PRIeid "\n", g.m);
    printf("diameter (brute force) : %" PRIdist "\n", diameter);
    printf("BFS calls              : %" PRIvid "\n", g.n);
    printf("time (s)               : %.3f\n", t1 - t0);

    int status = 0;
    if (have_expected) {
        printf("expected diameter      : %ld\n", expected);
        if ((long)diameter == expected) {
            printf("result                 : MATCH\n");
        } else {
            printf("result                 : MISMATCH\n");
            status = 2;
        }
    }

    csr_free(&g);
    return status;
}