#include <stdlib.h>     // malloc, calloc, free.
#include <stdio.h>      // fprintf.
#include "csr.h"        

void csr_free(csr_t *g) {
    free(g->row_ptr);
    free(g->col_idx);
    g->row_ptr = NULL;
    g->col_idx = NULL;
    g->n = 0;
    g->m = 0;
}

/* Helperes used only inside this file, not part of the API. */

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

static int parse_edge_line(const char **pp, const char *end,
                            vid_t n, vid_t *out_i, vid_t *out_j) {
    const char *p = *pp;

    uint64_t raw_i = parse_uint(&p, end);
    uint64_t raw_j = parse_uint(&p, end);

    if (raw_i == 0 || raw_j == 0 || raw_i > n || raw_j > n) {
        fprintf(stderr, "malformed or out-of-range entry: %llu %llu\n",
                (unsigned long long)raw_i, (unsigned long long)raw_j);
        return -1;
    }

    *out_i = (vid_t)(raw_i - 1);
    *out_j = (vid_t)(raw_j - 1);

    while (p < end && *p != '\n') {
        p++;
    }
    *pp = (p < end) ? p + 1 : end;

    return 0;
}