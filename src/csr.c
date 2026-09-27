/* csr.c -- builds the CSR (Compressed Sparse Row) graph from a parsed .mtx.
 *
 * Two passes over the memory-mapped file: pass 1 counts each vertex's degree
 * and a prefix sum turns the counts into row_ptr; pass 2 places every
 * neighbour into its slot of col_idx. A symmetric banner means the file
 * stores only one triangle, so each edge is inserted in both directions;
 * self-loops are dropped.
 *
 * csr_build_from_mtx() returns 0 and fills 'g' on success (the caller
 * releases it with csr_free()), or -1 on failure with 'g' left untouched. */

#include <stdlib.h>
#include <stdio.h>

#include "csr.h"

/* deallocates the variables and heap memory used for the csr format. */
void csr_free(csr_t *g) {
    free(g->row_ptr);
    free(g->col_idx);
    g->row_ptr = NULL;
    g->col_idx = NULL;
    g->n = 0;
    g->m = 0;
}

/* Reads one unsigned integer starting at *pp, skipping any leading
 * spaces/tabs first. */
static uint64_t parse_uint(const char **pp, const char *end) {
    const char *p = *pp;

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p++ - '0');
    }

    *pp = p;
    return v;
}


/* Reads one data line of the form "i j" (maybe followed by a value column
 * that is ignored), and:
 *   - converts from the file's 1-based indexing to 0-based vid_t
 *   - validates that both indices are in range
 *   - advances *pp past the entire line, ready for the next call
 *
 * Returns 0 on success, -1 if the line is malformed (an index is 0, meaning
 * the file is corrupt, or bigger than n, the declared matrix dimension). */
static int parse_edge_line(const char **pp, const char *end,
                            vid_t n, vid_t *out_i, vid_t *out_j) {
    const char *p = *pp;

    uint64_t raw_i = parse_uint(&p, end);
    uint64_t raw_j = parse_uint(&p, end);

    /* Matrix Market indices are 1-based by definition of the format, so a
     * 0 means a corrupt file. */
    if (raw_i == 0 || raw_j == 0 || raw_i > n || raw_j > n) {
        fprintf(stderr, "malformed or out-of-range entry: %llu %llu\n",
                (unsigned long long)raw_i, (unsigned long long)raw_j);
        return -1;
    }

    /* Now safe: convert to internal 0-based numbering. */
    *out_i = (vid_t)(raw_i - 1);
    *out_j = (vid_t)(raw_j - 1);

    /* Skip whatever is left on this line (a value column, if any): only
     * the graph's structure is needed, never edge weights. */
    while (p < end && *p != '\n') {
        p++;
    }
    *pp = (p < end) ? p + 1 : end;

    return 0;
}

/* Transformation of the .mtx data into csr format. */
int csr_build_from_mtx(const mtx_t *mx, csr_t *g) {
    const char *end = mx->base + mx->len;
    vid_t n = mx->n;


    /* PASS 1: count each vertex's degree.
     *
     * For each edge (i,j) i increment i's counter. If the file stores only
     * one triangle (is_symmetric), i also increment j's, since the edge ends
     * up in both adjacency lists. Otherwise the file already lists both
     * directions, and j's counter is incremented when its own line is read.
     *
     * calloc: the counters must start at zero. Size n+1 because this array
     * becomes row_ptr, whose extra last slot means the last vertex needs no
     * special case. */
    eid_t *degree = calloc((size_t)n + 1, sizeof(eid_t));
    if (!degree) {
        fprintf(stderr, "out of memory (degree array, %" PRIvid " vertices)\n", n);
        return -1;
    }

    const char *p = mx->data;       // mx->data points at the first character of the data.
    uint64_t line_no = 0;           // only used to make error messages useful.

    while (p < end) {
        vid_t i, j;
        if (parse_edge_line(&p, end, n, &i, &j) != 0) {
            fprintf(stderr, "  (at data line %" PRIu64 ")\n", line_no);
            free(degree);
            return -1;
        }
        line_no++;

        /* A self-loop (i == j) does not affect distances between different
         * vertices, so it is dropped (the line is still consumed). */
        if (i == j) continue;

        degree[i]++;
        if (mx->is_symmetric) degree[j]++;
    }

    /* PREFIX SUM: degree[] is transformed IN PLACE into row_ptr[]. */
    eid_t running = 0;
    for (vid_t v = 0; v <= n; v++) {
        eid_t d = degree[v];
        degree[v] = running;
        running += d;
    }
    /* After the loop, degree[n] == running == the grand total, which
     * is exactly m, the total number of directed entries in the CSR. */

    eid_t *row_ptr = degree;    // renaming for readability from here on.

    eid_t m = row_ptr[n];

    if (m == 0) {
        /* Every stored entry was a self-loop, or there were no off-diagonal
         * entries: no diameter can be computed. */
        fprintf(stderr, "graph has no edges after dropping self-loops\n");
        free(row_ptr);
        return -1;
    }

    vid_t *col_idx = malloc((size_t)m * sizeof(vid_t));
    if (!col_idx) {
        fprintf(stderr, "out of memory (col_idx, %" PRIeid " entries)\n", m);
        free(row_ptr);
        return -1;
    }

    /* PASS 2: place each neighbour into its slot in col_idx.
     *
     * row_ptr[v] says where v's block starts, but not how many of its slots
     * are already filled. scratch_buffer[v] (a copy of row_ptr) holds "the
     * next free slot of v". row_ptr itself cannot be the cursor: it would end
     * up shifted to row_ptr[v+1] and the offsets would be lost. */
    eid_t *scratch_buffer = malloc(((size_t)n + 1) * sizeof(eid_t));
    if (!scratch_buffer) {
        fprintf(stderr, "out of memory (scratch_buffer array)\n");
        free(row_ptr);
        free(col_idx);
        return -1;
    }
    for (vid_t v = 0; v <= n; v++) {
        scratch_buffer[v] = row_ptr[v];
    }

    p = mx->data;   /* REWIND to the first data line and re-parse the same
                     * bytes (already validated in pass 1). */

    while (p < end) {
        vid_t i, j;
        parse_edge_line(&p, end, n, &i, &j);

        if (i == j) continue;

        /* Write j into i's next free slot, then advance that slot by one. */
        col_idx[scratch_buffer[i]++] = j;

        /* If duplicating, also write the mirror entry i into j's list. */
        if (mx->is_symmetric) col_idx[scratch_buffer[j]++] = i;
    }

    free(scratch_buffer);

    /* From this point on, `g` owns row_ptr and col_idx. */
    g->n = n;
    g->m = m;
    g->row_ptr = row_ptr;
    g->col_idx = col_idx;
    return 0;
}
