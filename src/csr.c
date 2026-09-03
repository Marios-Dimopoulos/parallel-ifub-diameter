#include <stdlib.h>     // malloc, calloc, free
#include <stdio.h>      // fprintf
#include "csr.h"


void csr_free(csr_t *g) {
    free(g->row_ptr);
    free(g->col_idx);
    g->row_ptr = NULL;
    g->col_idx = NULL;
    g->n = 0;
    g->m = 0;
}


/* ==================================================================
 * Helpers used only inside this file, not part of the public API.
 * Neither is declared in csr.h -- callers outside this file have no
 * business calling them directly.
 * ================================================================== */

/* Reads one unsigned integer starting at *pp, skipping any leading
 * spaces/tabs first.
 *
 * Why the double pointer (const char **pp) instead of a plain
 * const char *p: a function in C can never modify the caller's own
 * variable, only what it points to. If we took a plain pointer, any
 * p++ inside this function would move a LOCAL COPY and the caller's
 * pointer would not budge -- the caller would re-read the same
 * number forever. Passing the address of the pointer (&p at the call
 * site) lets us write *pp = p at the end and really move the
 * caller's cursor forward.
 *
 * Example walk over "  42abc":
 *   skip two spaces
 *   see '4' -> v = 0*10 + 4 =  4
 *   see '2' -> v = 4*10 + 2 = 42
 *   see 'a' -> not a digit, loop stops
 *   *pp now points at 'a' */
static uint64_t parse_uint(const char **pp, const char *end) {
    const char *p = *pp;                              /* local working copy */

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;                                           /* eat leading whitespace */
    }

    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        /* '7' - '0' == 7 because ASCII digits are consecutive */
        v = v * 10 + (uint64_t)(*p++ - '0');
    }

    *pp = p;             /* write the new position back through the pointer */
    return v;
}


/* Reads one data line of the form "i j" (optionally followed by a
 * value column we don't care about, e.g. "i j 3.14"), and:
 *   - converts from the file's 1-based indexing to our 0-based vid_t
 *   - validates that both indices are in range
 *   - advances *pp past the entire line, ready for the next call
 *
 * Returns 0 on success, -1 if the line is malformed (index is 0,
 * meaning the file used 0-based numbering or is corrupt; or index
 * is bigger than n, the declared matrix dimension). */
static int parse_edge_line(const char **pp, const char *end,
                            vid_t n, vid_t *out_i, vid_t *out_j) {
    const char *p = *pp;

    uint64_t raw_i = parse_uint(&p, end);
    uint64_t raw_j = parse_uint(&p, end);

    /* Matrix Market indices are always 1-based by definition of the
     * format. raw_i == 0 would mean either a corrupt file or a bug
     * upstream of us -- either way, stop rather than let it silently
     * underflow into a huge number two lines below (raw_i - 1 on an
     * unsigned 0 would wrap to UINT64_MAX). */
    if (raw_i == 0 || raw_j == 0 || raw_i > n || raw_j > n) {
        fprintf(stderr, "malformed or out-of-range entry: %llu %llu\n",
                (unsigned long long)raw_i, (unsigned long long)raw_j);
        return -1;
    }

    /* Now safe: convert to our internal 0-based numbering. */
    *out_i = (vid_t)(raw_i - 1);
    *out_j = (vid_t)(raw_j - 1);

    /* Skip whatever is left on this line -- a value column, if the
     * file has one -- without caring what it contains. We only need
     * the graph's structure, never edge weights. */
    while (p < end && *p != '\n') {
        p++;
    }
    *pp = (p < end) ? p + 1 : end;   /* land at the start of the next line */

    return 0;
}


/* ==================================================================
 * csr_build_from_mtx -- the actual two-pass CSR construction.
 *
 * Overall plan:
 *   PASS 1        : count how many neighbours each vertex has
 *   PREFIX SUM    : turn those counts into row_ptr offsets
 *   PASS 2        : re-read the same lines, and this time actually
 *                   write each neighbour into its slot in col_idx
 * ================================================================== */
int csr_build_from_mtx(const mtx_t *mx, mtx_policy_t pol, csr_t *g) {
    const char *end = mx->base + mx->len;   /* one-past-the-last byte of the file */
    vid_t n = mx->n;                        /* number of vertices, already known
                                              * from the header mtx_open parsed */

    /* ============================================================
     * PASS 1: count each vertex's degree
     *
     * We walk every data line once. For each edge (i,j) we increment
     * a per-vertex counter. If we are duplicating (MTX_DUP, the file
     * stores only one triangle), we increment BOTH i's and j's
     * counters, since the edge will end up in both adjacency lists.
     * If the file already lists both directions (MTX_ASIS), we only
     * increment i's counter here -- the matching increment for j
     * will happen naturally when we later read the line that lists
     * the same edge from j's side.
     * ============================================================ */

    /* calloc, not malloc: every counter must start at exactly 0, and
     * calloc guarantees zeroed memory (plain malloc would hand back
     * whatever garbage bytes happened to be there, like we saw when
     * comparing malloc to mmap).
     *
     * Size is (n+1), not n: we are about to reuse this exact array
     * as row_ptr, which needs that extra trailing slot so the last
     * vertex doesn't need special-casing (see graph.h/csr.h). */
    eid_t *degree = calloc((size_t)n + 1, sizeof(eid_t));
    if (!degree) {
        fprintf(stderr, "out of memory (degree array, %" PRIvid " vertices)\n", n);
        return -1;
    }

    const char *p = mx->data;   /* cursor: starts right after the header,
                                  * at the first real data line */
    uint64_t line_no = 0;       /* only used to make error messages useful */

    while (p < end) {
        vid_t i, j;
        if (parse_edge_line(&p, end, n, &i, &j) != 0) {
            fprintf(stderr, "  (at data line %" PRIu64 ")\n", line_no);
            free(degree);
            return -1;
        }
        line_no++;

        /* A self-loop (i == j) contributes nothing to BFS distances
         * between DIFFERENT vertices, so we simply drop it here.
         * Note we still consumed the line above (p already advanced
         * past it) -- we just don't count it towards any degree. */
        if (i == j) continue;

        degree[i]++;
        if (pol == MTX_DUP) degree[j]++;
    }

    /* ============================================================
     * PREFIX SUM: degree[] is transformed IN PLACE into row_ptr[]
     *
     * This is the standard "exclusive prefix sum" pattern.
     *
     *   before:  degree = [ 2,  3,  3,  2,  ? ]   (raw counts, +1 slot)
     *   after :  degree = [ 0,  2,  5,  8, 10 ]   (now really row_ptr)
     *
     * How the loop produces that: `running` tracks "total edges seen
     * so far, before vertex v". For each v we:
     *   1. remember its old count in `d`  (we are about to overwrite it)
     *   2. write `running` into degree[v] -- this IS row_ptr[v]:
     *      the offset where vertex v's neighbours start
     *   3. add d onto running, so the NEXT vertex's offset already
     *      accounts for v's own neighbours
     *
     * Doing this inside the same array (instead of allocating a
     * second one) avoids a second allocation the size of the vertex
     * count -- cheap here, but the same trick matters a lot more
     * once n is in the hundreds of millions.
     * ============================================================ */
    eid_t running = 0;
    for (vid_t v = 0; v <= n; v++) {
        eid_t d = degree[v];   /* save v's raw degree before we overwrite it */
        degree[v] = running;   /* degree[v] now holds row_ptr[v] */
        running += d;          /* fold v's own edges into the running total */
    }
    /* After the loop, degree[n] == running == the grand total, which
     * is exactly m, the total number of directed entries in the CSR. */

    eid_t *row_ptr = degree;   /* pure renaming for readability from here on --
                                 * same array, same memory, new name because
                                 * its role has changed */
    eid_t m = row_ptr[n];

    if (m == 0) {
        /* Can happen if every stored entry was a self-loop, or the
         * matrix had zero off-diagonal entries -- not a graph we can
         * compute a diameter for. */
        fprintf(stderr, "graph has no edges after dropping self-loops\n");
        free(row_ptr);
        return -1;
    }

    /* Now, and only now, do we know m exactly, so we can allocate
     * col_idx at precisely the right size -- no over-allocation, no
     * resizing, no wasted memory on a multi-billion-entry array. */
    vid_t *col_idx = malloc((size_t)m * sizeof(vid_t));
    if (!col_idx) {
        fprintf(stderr, "out of memory (col_idx, %" PRIeid " entries)\n", m);
        free(row_ptr);
        return -1;
    }

    /* ============================================================
     * PASS 2: place each neighbour into its slot in col_idx
     *
     * row_ptr[v] tells us WHERE vertex v's block of neighbours
     * starts and ends, but by itself it does not tell us how many of
     * v's slots have already been filled while we walk through the
     * file. We need a second, throwaway array for that: cursor[v] =
     * "the next free slot for vertex v".
     *
     * It starts as an exact copy of row_ptr. Why not just use
     * row_ptr itself as the write position and fix it up afterwards?
     * Because by the time we finish, row_ptr[v] would have been
     * pushed all the way to row_ptr[v+1] -- we would have destroyed
     * the very offsets we need to return to the caller. A separate
     * small array (n+1 entries, negligible size) is simpler and
     * safer than trying to reconstruct row_ptr afterwards.
     * ============================================================ */
    eid_t *cursor = malloc(((size_t)n + 1) * sizeof(eid_t));
    if (!cursor) {
        fprintf(stderr, "out of memory (cursor array)\n");
        free(row_ptr);
        free(col_idx);
        return -1;
    }
    for (vid_t v = 0; v <= n; v++) {
        cursor[v] = row_ptr[v];
    }

    p = mx->data;   /* REWIND: go back to the very first data line and
                     * re-parse the exact same bytes we already read in
                     * pass 1. This is the "simple but re-reads the
                     * file twice" version we agreed to start with. */

    while (p < end) {
        vid_t i, j;
        /* Already validated in pass 1 -- if a line were malformed we
         * would have caught it and returned before ever reaching
         * pass 2, so we don't re-check the return value here. */
        parse_edge_line(&p, end, n, &i, &j);

        if (i == j) continue;   /* same self-loop skip as in pass 1 */

        /* Write j into i's next free slot, then advance that slot by one. */
        col_idx[cursor[i]++] = j;

        /* If duplicating, also write the mirror entry i into j's list. */
        if (pol == MTX_DUP) col_idx[cursor[j]++] = i;
    }

    /* ============================================================
     * SANITY CHECK: did we fill exactly as many slots as we counted?
     *
     * After pass 2, cursor[v] should have been incremented exactly
     * deg(v) times, landing precisely on row_ptr[v+1] -- the start of
     * the NEXT vertex's block. If it landed anywhere else, pass 1 and
     * pass 2 disagreed about how many edges vertex v has.
     *
     * This should be mathematically impossible, since both passes
     * parse the exact same bytes with the exact same logic. If this
     * ever fires, it points to a genuine bug (e.g. a stray global
     * state, an off-by-one somewhere) rather than bad input -- pass 1
     * already rejected malformed lines before we ever got here.
     * ============================================================ */
    for (vid_t v = 0; v < n; v++) {
        if (cursor[v] != row_ptr[v + 1]) {
            fprintf(stderr, "internal error: cursor mismatch at vertex %"
                    PRIvid "\n", v);
            free(row_ptr);
            free(col_idx);
            free(cursor);
            return -1;
        }
    }
    free(cursor);   /* purely scratch space, not returned to the caller */

    /* Hand the finished graph back to the caller. From this point on,
     * `g` owns row_ptr and col_idx, and is responsible for eventually
     * calling csr_free() on it. */
    g->n = n;
    g->m = m;
    g->row_ptr = row_ptr;
    g->col_idx = col_idx;
    return 0;
}