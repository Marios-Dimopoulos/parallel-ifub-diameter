#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <omp.h>

#include "mtx.h"
#include "csr.h"
#include "two_sweep.h"
#include "ifub.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-v] <file.mtx>\n"
                    "   -v    print interlan iFUB progress diagnostics to stderr\n", prog);
}

int main(int argc, char **argv) {
    int verbose = 0;
    const char *path = NULL;

    /* minimal argument parsing: [-v] <file.mtx> */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!path) {
        usage(argv[0]);
        return 1;
    }
    ifub_verbose = verbose;

    int nthreads = omp_get_max_threads();

    /* Phase 1: I/O + CSR construction. Timed separately, and
     * NEVER counted towards "diameter computation time" -- this
     * phase is inherently sequential and not part of what i'm 
     * parallelising or reporting speedup for. */
    double t_io_start = now();

    mtx_t mx;
    if (mtx_open(path, &mx) !=0) {
        fprintf(stderr, "failed to open %s\n", path);
        return 1;
    }
    if (verbose) {
        mtx_describe(&mx);
    }

    csr_t g;
    if (csr_build_from_mtx(&mx, &g) !=0) {
        fprintf(stderr, "failed to build CSR from %s\n", path);
        mtx_close(&mx);
        return 1;
    }
    mtx_close(&mx);

    double t_io_end = now();

    /* Phase 2: two-sweep + iFUB. This is the block whose time 
     * belongs in speedup/efficiency plots. */
    dist_t *dist = malloc((size_t)g.n * sizeof(dist_t));
    if (!dist) {
        fprintf(stderr, "out of memory (dist array)\n");
        csr_free(&g);
        return 1;
    }

    double t_compute_start = now();

    dist_t lb0;
    vid_t u = two_sweep(&g, dist, &lb0);

    uint64_t bfs_count = 0;
    dist_t diameter = ifub_diameter(&g, u, lb0, dist, &bfs_count);

    double t_compute_end = now();

    /* Report. One line per run, easy to grep/parse from a 
     * shell loop when sweeping over OMP_NUM_THREADS. */
    printf("graph                  : %s\n", path);
    printf("threads                : %d\n", nthreads);
    printf("vertices                : %" PRIvid "\n", g.n);
    printf("edges (directed entries): %" PRIeid "\n", g.m);
    printf("two-sweep start vertex : %" PRIvid "\n", u);
    printf("two-sweep lower bound  : %" PRIdist "\n", lb0);
    printf("diameter               : %" PRIdist "\n", diameter);
    printf("total BFS calls        : %" PRIu64 "\n", bfs_count);
    printf("io+csr time (s)        : %.3f\n", t_io_end - t_io_start);
    printf("diameter time (s)      : %.3f\n", t_compute_end - t_compute_start);

    /* Machine-readable summary line, convenient for scripts building 
     * scaling tables: graph, threads, diameter, bfs_count, compute_time */
   printf("CSV,%s,%d,%" PRIvid ",%" PRIeid ",%" PRIvid ",%" PRIdist ",%" PRIdist ",%" PRIu64 ",%.3f,%.3f\n",
            path, nthreads, g.n, g.m, u, lb0, diameter, bfs_count,
            t_io_end - t_io_start, t_compute_end - t_compute_start);

    free(dist);
    csr_free(&g);
    return 0;
}