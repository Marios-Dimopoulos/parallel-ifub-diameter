/* mtx.c -- opens a Matrix Market (.mtx) file and reads only its header.
 *
 * The file is memory-mapped (mmap); the banner and the dimension line are
 * parsed and the answers are returned in an mtx_t: how many vertices there
 * are, where the edge lines start, whether each stored entry means one edge
 * or two (symmetric banner), and whether there is a value column to skip.
 * No edge is read here -- csr.c does that. */

/* Feature test macro: some system functions (e.g. madvise) are Linux
 * extensions that headers hide unless this is defined. It MUST come before
 * any #include, or the declaration is lost ("implicit declaration of
 * madvise"). */
#define _GNU_SOURCE

#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "mtx.h"

/* Moves a pointer to the start of the next line. The (p < end) check is
 * needed because this walks an mmap'd region, not a C string: there is no
 * '\0' to stop at, so a file without a final newline would otherwise run
 * past the end of the mapping (segfault). */
static const char *skip_line(const char *p, const char *end) {
    while (p < end && *p != '\n') {
        p++;
    }
    return (p < end) ? p + 1 : end;
}

int mtx_open(const char *path, mtx_t *mx) {
    /* STEP 1: Open the file. */

    /* open() instead of fopen(): mmap() below needs an int file descriptor,
     * and FILE* exists to buffer (copy) data, the opposite of what i want. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
       perror("open");
       return -1;
    }

    /* STEP 2: Find out how big the file is. */

    /* struct stat holds a file's METADATA -- not its contents. */
    struct stat st;

    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }

    if (st.st_size == 0) {
        fprintf(stderr, "empty file\n");
        close(fd);
        return -1;
    }

    /* STEP 3: Map the file into memory. */

    /* mmap() reads nothing by itself: it only tells the kernel that the
     * addresses base .. base+size correspond to this file. Touching base[...]
     * later raises a page fault and the kernel loads that chunk, so a file of
     * tens of GB behaves like an ordinary char array.
     * Arguments: NULL (kernel picks the address), the size, PROT_READ
     * (read-only), MAP_PRIVATE (never modify the file), fd, offset 0. */
    const char *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    /* The mapping keeps its own reference to the file. */
    close(fd);

    if (base == MAP_FAILED) {
       perror("mmap");
       return -1;
    }

    /* STEP 4: Hint the kernel about the access pattern. */

    /* Advice only: it does not affect correctness and the kernel may ignore
     * it, so the return values are not checked.
     *  MADV_SEQUENTIAL: in-order access is expected -> more aggressive
     *                   read-ahead, and pages already passed may be freed
     *                   sooner.
     *  MADV_WILLNEED:   start reading the file in the background instead of
     *                   waiting for page faults.
     * The data lives on /scratch, a network filesystem, where fewer round
     * trips help. */
    madvise((void *)base, (size_t)st.st_size, MADV_SEQUENTIAL);
    madvise((void *)base, (size_t)st.st_size, MADV_WILLNEED);

    mx->base = base;
    mx->len = (size_t)st.st_size;

    /* `p` is the moving cursor, `end` bounds the region i may touch. */
    const char *p = base;
    const char *end = base + mx->len;

    /* STEP 5: Check if this is really a Matrix Market file.*/

    /* Reject anything too small or wrong before trusting its bytes. The
     * length check must come first: strncmp() reads 14 bytes, which would run
     * past the mapping of a tiny file. */
    if (mx->len < 15 || strncmp(p, "%%MatrixMarket", 14) != 0) {
        fprintf(stderr, "not a Matrix Market file\n");
        munmap((void *)base, mx->len);      // Undo the mmap before leaving.
        return -1;
    }

    /* STEP 6: Locate the end of the first line.*/

    /* memchr (unlike strchr) does not rely on '\0', which is what an mmap
     * needs. It gives the end of the banner line (nl):
     *
     *  %%MatrixMarket matrix coordinate pattern symmetric\n% test...
     *                                                     ^
     *                                                     nl
     *
     * used for the banner's length (nl - p) and to continue after it
     * (nl + 1). */
    const char *nl = memchr(p, '\n', mx->len);
    if (!nl) {
        munmap((void *)base, mx->len);
        return -1;
    }

    /* STEP 7: copy the banner out and lowercase it. */

    /* The mapping is read-only and shared with the page cache, so it cannot
     * be lowercased in place: copy the banner into a local buffer for
     * case-insensitive searching. */
    size_t blen = (size_t)(nl - p);     // Length of the first line.
    if (blen > 255) blen = 255;

    char banner[256];
    memcpy(banner, p, blen);
    banner[blen] = '\0';    // Now it is a proper C string.

    for (size_t i = 0; i < blen; i++) {
        banner[i] = (char)tolower((unsigned char)banner[i]);
    }

    /* STEP 8: Decode the banner. */

    /* "coordinate" = a list of "i j" entries (sparse). The alternative,
     * "array", is a dense matrix; only sparse is supported. */
    if (!strstr(banner, "coordinate")) {
       fprintf(stderr, "only coordinate format supported\n");
       munmap((void *)base, mx->len);
       return -1;
    }

    /* Complex values would put two numbers in the value column and break
     * the line parsing later. */
    if (strstr(banner, "complex")) {
        fprintf(stderr, "complex values not supported\n");
        munmap((void *)base, mx->len);
        return -1;
    }

    /* "pattern" means there is no value column, just "i j". Otherwise each
     * line is "i j value" and the value is skipped. */
    mx->is_pattern = (strstr(banner, "pattern") != NULL);

    /* "symmetric" means only ONE triangle is stored, so a stored entry
     * (i, j) also implies (j, i) and both directions must be inserted later.
     * "skew-symmetric" contains "symmetric" as a substring, which is fine:
     * it also stores one triangle and only the structure matters. The same
     * goes for "hermitian". */
    mx->is_symmetric = (strstr(banner, "symmetric") != NULL) ||
                       (strstr(banner, "hermitian") != NULL);

    /* STEP 9: Walk past the comment lines. */

    /* Skip comment lines ('%') and blank lines after the banner; '\r' covers
     * Windows-style "\r\n" line ends. */
    p = nl + 1;
    while (p < end && (*p == '%' || *p == '\n' || *p == '\r')) {
        p = skip_line(p, end);
    }
    /* p now points at the dimension line. */

    /* STEP 10: Read the three dimension numbers. */

    /* The line is "rows columns entries", parsed digit by digit instead of
     * with sscanf. */

    uint64_t dims[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {

       /* eat any leading spaces or tabs before the number */
       while (p < end && (*p == ' ' || *p == '\t')) {
           p++;
       }

       uint64_t v = 0;
       while (p < end && *p >= '0' && *p <= '9') {
           v = v * 10 + (uint64_t)(*p++ - '0');
       }
       dims[k] = v;
    }

    /* Move past the rest of the dimension line. After this, p points at the
     * first real data line. */
    p = skip_line(p, end);

    /* STEP 11: Sanity checks on the dimensions. */

    /* An adjacency matrix is always square. */
    if (dims[0] != dims[1]) {
        fprintf(stderr, "matrix is not square: %llu x %llu\n",
                (unsigned long long)dims[0], (unsigned long long)dims[1]);
        munmap((void *)base, mx->len);
        return -1;
    }

    if (dims[0] == 0) {
        fprintf(stderr, "zero-dimension matrix\n");
        munmap((void *)base, mx->len);
        return -1;
    }

    /* vid_t is uint32_t: more than (2^32)-1 vertices cannot be represented. */
    if (dims[0] > VID_MAX) {
        fprintf(stderr, "n = %llu exceeds 32-bit vid_t\n",
                (unsigned long long)dims[0]);
        munmap((void *)base, mx->len);
        return -1;
    }

    /* STEP 12: record the answers and return. */

    mx->n = (vid_t)dims[0];
    mx->data = p;
    mx->nnz_lines = dims[2];

    return 0;
}

/* Release the mapping. */
void mtx_close(mtx_t *mx) {
    if (mx->base) {
        munmap((void *)mx->base, mx->len);
        mx->base = NULL;
        mx->data = NULL;
    }
}

/* Prints human-friendly information about the parsed header to stderr, not
 * stdout, so the program's real output can be redirected without
 * diagnostics mixed into it. */
void mtx_describe(const mtx_t *mx) {
    fprintf(stderr,
        "file    : %.2f GB\n"
        "vertices: %" PRIvid "\n"
        "entries : %" PRIeid "\n"
        "format  : %s, %s\n",
        (double)mx->len / (1024.0 * 1024.0 * 1024.0),
        mx->n,
        mx->nnz_lines,
        mx->is_pattern   ? "pattern"   : "with values",
        mx->is_symmetric ? "symmetric" : "general");
}
