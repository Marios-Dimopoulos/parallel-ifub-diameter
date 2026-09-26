/* main.c -- command-line entry point of the iFUB diameter tool.
 *
 * Pipeline: (1) mmap and parse the .mtx header (mtx.c), (2) build the CSR
 * graph (csr.c), (3) pick the start vertex and a lower bound with two-sweep
 * (two_sweep.c), (4) run the parallel iFUB algorithm, which returns the exact
 * diameter (ifub.c), (5) print a human-readable report and one "CSV,..." line
 * that the sweep scripts collect. The I/O + CSR phase and the diameter phase
 * are timed separately.
 *
 * Usage: ./ifub [-v] <file.mtx>
 *      -v prints ifub_diameter()'s [debug]/[progress] diagnostics to stderr
 *      (off by default: a scaling sweep runs the program many times). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <omp.h>

#include "mtx.h"
#include "csr.h"
#include "two_sweep.h"
#include "ifub.h"

/* Wall-clock time in seconds. CLOCK_MONOTONIC (not CLOCK_REALTIME) is used
 * because it only moves forward and ignores system clock adjustments, which
 * would corrupt the measurement of long runs. */
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Prints the command-line usage to stderr. */
static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-v] <file.mtx>\n"
                    "   -v    print internal iFUB progress diagnostics to stderr\n", prog);
}

int main(int argc, char **argv) {
    int verbose = 0;
    const char *path = NULL;

    /* Hand-rolled parsing of [-v] <file.mtx> (getopt would be overkill for
     * one flag and one path). "-v" is accepted anywhere; the first other
     * argument is the graph file and a second one is rejected with the
     * usage message. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (path == NULL) {
            path = argv[i];     // first non-flag argument is the graph file.
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!path) {
        usage(argv[0]);     // No filename was given at all.
        return 1;
    }

    /* Global flag declared in ifub.h, defined in ifub.c, read there by
     * every diagnostic message. */
    ifub_verbose = verbose;

    /* Number of threads OpenMP will use (OMP_NUM_THREADS, or the core count
     * if unset). Printed in the report and in the CSV line, so a script
     * sweeping over thread counts does not have to track it. */
    int nthreads = omp_get_max_threads();

    /* Phase 1: I/O + CSR construction. Timed separately from Phase 2 on
     * purpose: it is sequential work, so adding it to the diameter time would
     * distort the speedup of the part that IS parallelised. */
    double t_io_start = now();

    mtx_t mx;
    if (mtx_open(path, &mx) != 0) {
        /* mtx_open() already printed the low-level reason; this adds WHICH
         * file failed, useful when a script runs many graphs. */
        fprintf(stderr, "failed to open %s\n", path);
        return 1;
    }
    if (verbose) {
        mtx_describe(&mx);
    }

    csr_t g;
    if (csr_build_from_mtx(&mx, &g) != 0) {
        fprintf(stderr, "failed to build CSR from %s\n", path);
        mtx_close(&mx);     // Release the mmap even on this failure path.
        return 1;
    }
    /* The CSR is self-contained now, so the mapping can be released right
     * away instead of being held for the whole (possibly very long) run. */
    mtx_close(&mx);

    double t_io_end = now();

    /* Phase 2: two-sweep + iFUB. Only ifub_diameter()'s fringe loop is
     * parallelised with OpenMP, so this is the time the speedup plots use. */

    /* Scratch array for two_sweep() and for the first BFS inside
     * ifub_diameter(); the parallel BFS calls there use their own per-thread
     * arrays (see ifub.c). */
    dist_t *dist = malloc((size_t)g.n * sizeof(dist_t));
    if (!dist) {
        fprintf(stderr, "out of memory (dist array)\n");
        csr_free(&g);
        return 1;
    }

    double t_compute_start = now();

    /* Step A: pick the start vertex u (the midpoint of a long path, i.e. a
     * central vertex) and an initial lower bound lb0, with two sequential
     * BFS calls. See two_sweep.c. */
    dist_t lb0;
    vid_t u = two_sweep(&g, dist, &lb0);

    /* Step B: the parallel iFUB. bfs_count receives the number of BFS calls
     * its fringe loop performed -- a useful diagnostic, from none up to
     * almost one per vertex depending on the graph. */
    uint64_t bfs_count = 0;
    dist_t diameter = ifub_diameter(&g, u, lb0, dist, &bfs_count);

    /* ifub_diameter() returns -1 (after printing its own reason to stderr)
     * on failure. Without this check the report and the CSV line would show
     * "diameter : -1" as if it were a result, and the program would exit 0. */
    if (diameter == -1) {
        fprintf(stderr, "failed to compute diameter for %s\n", path);
        free(dist);
        csr_free(&g);
        return 1;
    }

    double t_compute_end = now();

    /* Report, both on stdout: labelled lines for a human, and one "CSV,..."
     * line that the sweep scripts grep into their results file. */
    printf("graph                  : %s\n", path);
    printf("threads                : %d\n", nthreads);
    printf("vertices               : %" PRIvid "\n", g.n);
    printf("edges(directed entries): %" PRIeid "\n", g.m);
    printf("two-sweep start vertex : %" PRIvid "\n", u);
    printf("two-sweep lower bound  : %" PRIdist "\n", lb0);
    printf("diameter               : %" PRIdist "\n", diameter);
    printf("total BFS calls        : %" PRIu64 "\n", bfs_count);
    printf("io+csr time (s)        : %.3f\n", t_io_end - t_io_start);
    printf("diameter time (s)      : %.3f\n", t_compute_end - t_compute_start);

    /* The column order must match the header row that the sweep scripts
     * write at the top of their CSV files:
     *   graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,
     *   diameter,bfs_count,io_csr_time,diameter_time */
    printf("CSV,%s,%d,%" PRIvid ",%" PRIeid ",%" PRIvid ",%" PRIdist ",%" PRIdist ",%" PRIu64 ",%.3f,%.3f\n",
           path, nthreads, g.n, g.m, u, lb0, diameter, bfs_count,
           t_io_end - t_io_start, t_compute_end - t_compute_start);

    free(dist);
    csr_free(&g);
    return 0;
}
