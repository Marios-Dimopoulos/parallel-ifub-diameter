/* main.c -- command-line entry point for the iFUB diameter tool.
 *
 * End-to-end pipeline:
 *      1. mmap + parse the .mtx header (mtx.c)
 *      2. build the CSR graph, two passes over the file (csr.c)
 *      3. two-sweep: pick a good starting vertex + a lower bound 
 *      4. ifub_diameter: the parallel iFUB algorithm itself,
 *         returns the EXACT diameter.
 *      5. print a human-readable report AND one machine-readable
 *         "CSV,..." line, so this same binary works both for 
 *         one-off manual runs and for shell scripts sweeping over
 *         OMP_NUM_THREADS values.
 * 
 * Usage: ./ifub [-v] <file.mtx>
 *      -v turns on ifub_diameter()'s internal [debug]/[progress]
 *      diagnostics (see ifub.c) -- OFF by default, since a full
 *      scaling sweep produces one run per (graph, thread cound)
 *      pair and verbose output from every one of them would be 
 *      unreadable noise. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <omp.h>

#include "mtx.h"
#include "csr.h"
#include "two_sweep.h"
#include "ifub.h"

/* now() -- current wall-clock time, in fractional seconds.
 *
 * CLOCK_MONOTONIC (rather than CLOCK_REALTIME /time()) is used
 * deliberately: it is guaranteed to only ever move forward, and is
 * immune to the system clock being adjusted mid-run (NTP sync,
 * daylight saving, manual clock changes). For measuring durations 
 * of minutes-to-hours-long runs on a shared cluster, that immunity 
 * matters -- CLOCK_REALTIME jumping backward or forward mid-measurement
 * would silently produce a wrong, or even negative, duration. */
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Prints correct command-line usage to stderr. Called whenever the
 * arguments given don't match what main() expects, so the person
 * running the program (possibly future-me, having forgotten the 
 * exact flags) gets and immediate, self-contained reminder instead 
 * of a confusing crash or silent wrong behavior. */
static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-v] <file.mtx>\n"
                    "   -v    print internal iFUB progress diagnostics to stderr\n", prog);
}

int main(int argc, char **argv) {
    int verbose = 0;
    const char *path = NULL;

    /* Minimal hand-rolled argument parsing: [-v] <file.mtx>.
     * 
     * Deliberately not using getopt() here -- with only one optional
     * flag and one required positional argument, a full option-
     * parsing lirary would be more machinery than the problem
     * needs. The loop below accepts "-v" in any position (before 
     * or after the filename) and rejects anything beyond one flag plus
     * one path (e.g. two filenames, or an unrecognised flag) by
     * falling through to the usage() + exit(1) branch. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (path == NULL) {
            path = argv[i];     // fist non-flag argument is the graph file.
        } else {
            usage(argv[0]);     // A second non-flag argument -- reject it.
            return 1;
        }
    }

    if (!path) {
        usage(argv[0]);     // No filename was given at all.
        return 1;
    }

    /* ifub_verbose is a global flag declared in ifub.h and defined 
     * in ifub.c. Setting it here, before any call into that file, 
     * is what turns the -v flag into ifub_diameter()'s actual
     * [debug]/[progress] stderr output -- see ifub.c for every place
     * that checks it. */
    ifub_verbose = verbose;

    /* Captured once, up front: this is how many threads OpenMP will
     * actually use for every #pragma omp parallel region inside
     * ifub_diameter(), decided by the OMP_NUM_THREADS enviroment
     * variable (or the OS's default core count, if that variable is
     * unset). Printed in both the human-readable report and the CSV
     * line below, so a shell script sweeping over thread counts
     * doesn't need to separately track which run used how many. */
    int nthreads = omp_get_max_threads();

    /* Phase 1: I/O + CSR construction. 
     * 
     * Timed SEPARATELY from Phase 2, and this timing is NEVER added
     * into the "diameter computation time" reported further down.
     * This is a deliberate methodological choice: mmap'ing the file
     * and building the CSR structure is inherently sequential work
     * (see mtx.c / csr.c) -- it is not something this project
     * parallelises, and it is not what the speedup/efficiency plots
     * in the report are about. Mixing the two timings together would
     * artificially shrink the apparent speedup of the part that 
     * actually IS parallelised, just because a fixed, non-parallel
     * I/O cost got averaged into it. (t_io_end - t_io_start is really
     * small value compared to t_compute_end - t_compute_start. It wouldn't 
     * matter that much if the first was included in the second. But for being 
     * professionals, i do it the right way.). */
    double t_io_start = now();

    mtx_t mx; 
    if (mtx_open(path, &mx) != 0) {
        /* mtx_open() already printed its own lower-level reason
         * (e.g. via perror(), if a system call like open()/mmap()
         * failed). This second message adds the one piece of 
         * context that lower-level message can't know on its own:
         * WHICH file, in THIS specific run of the program, was the 
         * one that failed -- useful when this binary is invoked
         * many times over many different graphs from a shell
         * script, and only stdout/stderr (not the exact command
         * line) ends up in a saved log. */
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
    /* The CSR (g) is now fully self_contained in its own malloc'd 
     * arrays (row_ptr, col_idx) -- it no longer needs the original
     * .mtx file's mapping at all. Closing it here, as soon as 
     * possible, frees that mapping/address space promptly rather
     * than holding it open uselessly for the entire rest of the 
     * program's (potentially very long) run. */
    mtx_close(&mx);

    double t_io_end = now();

    /* Phase 2: two-sweep + iFUB. 
     * 
     * THIS is the block whose elapsed time belongs in speedup and 
     * efficiency plots -- it is the only part of the program that
     * is actually parallelised with OpenMP (inside ifub_diameter()'s
     * fringe loop), so it is the only part where "more threads"
     * should be expected to produce a measurable difference at all. */
    
    /* This allocated array is needed for mainly for two_sweep(...)
     * and for the first BFS call that is made inside ifub_diameter(...).
     * After that, in the body of ifub_diameter(...), there is separate
     * space allocated for every thread that will be executed. Each and every
     * thread needs itw own private scratch space in order for the algorithm
     * to run correctly. (see ifub.c). */
    dist_t *dist = malloc((size_t)g.n * sizeof(dist_t));
    if (!dist) {
        fprintf(stderr, "out of memory (dist array)\n");
        csr_free(&g);
        return 1;
    }

    double t_compute_start = now();

    /* Step A: pick a good starting vertex u (typically a peripheral
     * vertex, far from the graph's "centre") plus an initial lower 
     * bound lb0 on the diameter, using two sequential BFS calls. See 
     * two_sweep.c for the full algorithm. */
    dist_t lb0;
    vid_t u = two_sweep(&g, dist, &lb0);
    
    /* Step B: the parallel iFUB algorithm itself. bfs_count is 
     * filled in by ifub_diameter() with the EXACT number of BFS
     * calls its fringe loop actually performed -- usefull on its own
     * as a diagnostic (see the exploratory runs recorded in this 
     * project's notes: some graphs need only a few dozen BFS calls
     * to converge, other need almost one per vertex). */
    uint64_t bfs_count = 0;
    dist_t diameter = ifub_diameter(&g, u, lb0, dist, &bfs_count);

    double t_compute_end = now();

    /* Report
     *
     * Two different outputs, both to stdout, for two different 
     * audiences:
     *      - the labelled "key : value" lines below are for a HUMAN
     *        reading one run's output directly in a terminal
     *      - the single "CSV,..." line further down is for A SCRIPT
     *        collectin many runs' results into one table (see
     *        ifub_scaling_sweep.sh, which greps for lines starting
     *        with "CSV," and appends them, minus that prefix, to a 
     *        master results file)
     * Both come from the exact same run -- there is no second
     * invocation of the program anywhere to produce the CSV line. */
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

    /* Machine-readable summary line. Column order here must stay in
     * sync with the header row that ifub_scaling_sweep.sh writes
     * once at the top of its master CSV file:
     *   graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,
     *   diameter,bfs_count,io_csr_time,diameter_time
     * If a field is ever added/removed/reordered here, the header
     * line in that script must be updated to match, or the columns
     * will silently no longer line up with their labels. */
    printf("CSV,%s,%d,%" PRIvid ",%" PRIeid ",%" PRIvid ",%" PRIdist ",%" PRIdist ",%" PRIu64 ",%.3f,%.3f\n",
           path, nthreads, g.n, g.m, u, lb0, diameter, bfs_count,
           t_io_end - t_io_start, t_compute_end - t_compute_start);

    free(dist);
    csr_free(&g);
    return 0;
}