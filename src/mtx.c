#define _GNU_SOURCE     // must come before any #include: unlocks madvise().

#include <fcntl.h>      // open, O_RDONLY
#include <unistd.h>     // close
#include <ctype.h>      // tolower
#include <string.h>     //strncmp, strstr, memchr
#include <stdio.h>      //fprintf, perror
#include <sys/mman.h>   // mmap, munmap, madvise
#include <sys/stat.h>   // fstat, struct stat

#include "mtx.h"

// Advance past the next newline. Returns 'end' if there is none,
// which keeps every caller inside the mapping.
static const char *skip_line(const char *p, const char *end) {
    while (p < end && *p != '\n') p++;
    return (p < end) ? p + 1 : end;
}

int mtx_open(const char *path, mtx_t *mx) {
    inf fd = open(path, o_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

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

    const char *base = mmap(NULL, (size_t)st.st_size,
                            PROT_READ, MAP_PRIVATE, fd, 0);

    // The mapping holds its own reference to the file, so the 
    // descriptor is no longer needed.
    close(fd);

    if (base == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    // Tell the kernel how we intend to read: front to back, all of it.
    // This switches on aggressive readahead, which matters a lot when
    // the file lives on NFS and each fault is a network round trip.
    madvise((void *)base, (size_t)st.st_size, MADV_SEQUENTIAL);
    madvise((void *)base, (size_t)st.st_size, MADV_WILLNEED);

    mx->base = base;
    mx->len = (size_t)st.st_size;

    const char *p = base;
    const char *end = base + mx->len;

    if (mx->len < 15 || strncmp(p, "%%MatrixMarket", 14) != 0) {
        fprintf(stderr, "not a Matrix Market file\n");
        munmap((void *)base, mx->len);
        return -1;
    }

    const char *nl = memchr(p, '\n', mx->len);
    if (!nl) {
        munmap((void *)base, mx->len);
        return -1;
    }

    // Copy the banner into a small buffer and lowercase it, so the 
    // strstr() checks below are case-insensitive. We cannot modify the
    // mapping itself -- it is read-only, and shared with the page cace.
    size_t blen = (size_t)(nl - p);
    if (blen > 255) blen = 255;

    char banner[256];
    memcpy(banner, p, blen);
    banner[blen] = '\0';

    for (size_t i = 0; i < blen; i++) {
        banner[i] = (char)tolower((unsigned char)banner[i]);
    }

    if (!strstr(banner, "coordinate")) {
        fprintf(stderr, "only coordinate format supported\n");
        munmap((void *)base, mx->len);
        return -1;
    }
    if (strstr(banner, "complex")) {
        fprintf(stderr, "complex values not supported\n");
        munmap((void *)base, mx->len); 
        return -1;
    }

    mx->is_pattern = (strstr(banner, "pattern") != NULL);

    mx->is_symmetric = (strstr(banner, "symmetric") != NULL) ||
                       (strstr(banner, "hermitian") != NULL);

    // Skip the banner line, then any number of comment or blank lines.
    p = nl + 1;
    while (p < end && (*p == '%' || *p == '\n' || *p == '\r')) {
        p = skip_line(p, end);
    }

    // The dimension line: rows, columns, entry count.
    uint64_t dims[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        uint64_t v = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            v = v * 10 + (uint64_t)(*p++ - '0');
        }
        dims[k] = v;
    }
    p = skip_line(p, end);

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
    if (dims[0] > VID_MAX) {
        fprintf(stderr, "n = %llu exceeds 32-bit vid_t\n",
                (unsigned long long)dims[0]);
        munmap((void *)base, mx->len);
        return -1;
    }

    mx->n = (vid_t)dims[0];
    mx->nnz_lines = dims[2];
    mx->data = p;
    return 0;
}

void mtx_close(mtx_t *mx) {
    if (mx->base) {
        munmap((void *)mx->base, mx->len);
        mx->base = NULL;
        mx->data = NULL;
    }
}

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

mtx_policy_t mtx_policy(const mtx_t *mx) {
    return mx->is_symmetric ? MTX_DUP : MTX_ASIS;
}