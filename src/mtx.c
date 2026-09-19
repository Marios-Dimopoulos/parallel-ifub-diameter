/* mtx.c -- opening a Matrix Market file and reading its header
 * 
 * This file does exactly ONE job: given a filename, it answers
 * four questions and then stops.
 *
 *  1. How many vertices does the graph have?
 *  2. Where in the file do the actual edges start?
 *  3. Does each stored entry mean one edge or two?
 *  4. Is there a third column of values to skip?
 *
 * It does NOT read a single edge.
 * Think of it as opening a book and reading only the table of 
 * contents to find the page where the story begins. */

/* A "feature test macro". System headers hide some functions behind
 * #ifndef checks, because they are Linux extensions and not part of 
 * standard C. Defining this switches them on. It MUST be the very first line.
 * If it comes after an #include, that header was already processed with the switch off,
 * and the function declaration is lost -- you get "implicit declaration of madvise". */
#define _GNU_SOURCE

#include <fcntl.h>      // POSIX file control: open(), O_RDONLY 
#include <unistd.h>     // POSIX misc: close() 
#include <ctype.h>      // standard C character tools: tolower()    
#include <string.h>     // standard C: strncmp, strstr, memchr, memcpy 
#include <stdio.h>      // standard C: fprintf, perror 
#include <sys/mman.h>   // POSIX memory mapping: mmap, munmap, madvise 
#include <sys/stat.h>   // POSIX file metadata: fstat, struct stat 

#include "mtx.h"

/* Move a pointer to the start of the next line.
 * Why the (p < end) check on every step: it is walking over an
 * mmap'd region, not a C string. There is no terminating '\0' to
 * stop the program. If the file ended without a newline and it was
 * not checked, it would walk pass the end of the mapping, witch means segfault */
static const char *skip_line(const char *p, const char *end) {
    while (p < end && *p != '\n') {
        p++;
    }
    return (p < end) ? p + 1 : end;
}

int mtx_open(const char *path, mtx_t *mx) {
    /* STEP 1: Open the file. */

    /* open() is the raw system call. I use it instead of the more 
     * familiar fopen() for two reasons:
     *  - mmap() bellow requires an int file descriptor, not a FILE*
     *  - FILE* exists to give us a buffer that copies data around. 
     *    I want the exact opposite, zero copies.
     * It returns a small integer, the "handle" for this open file. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
       perror("open");
       return -1;
    }

    /* STEP 2: Find out how big the file is. */

    /* struct stat holds a file's METADATA -- not its contents. */
    struct stat st;

    /* fstat() fills the struct for an already-open descriptor. */
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

    /* This is the key line of the whole file
     * 
     * mmap() does NOT read anything. Zero bytes come off the disk
     * here. All it does is tell the kernel: "the addresses from 
     * base to base + size now correspond to this file
     * 
     * The payoff come afterwards. When i later touch base[...],
     * the CPU notices that page is not loaded and raises a page
     * fault. The kernel catches it, sees the note it made here,
     * fetches that chunk from disk, and resumes us. The code never 
     * notices. So big file (tens of GB) becomes an ordinary char array.
     * 
     * Arguments:
     *  NULL        -> kernel picks the address
     *  st.st_size  -> how many bytes to map
     *  PROT_READ   -> read-only (matches our const char *)
     *  MAP_PRIVATE -> Never change the file
     *  fd, 0       -> which file, starting at which offset*/
    const char *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    /* The mapping keeps its own reference to the file. */
    close(fd);

    if (base == MAP_FAILED) {
       perror("mmap");
       return -1;
    }

    /* STEP 4: Hint the kernel about the access pattern. */
     
    /* These are advice only. They change nothing about correctness,
     * only the kernel's strategy. It is free to ignore them, which
     * is why i do not check the return value.
     *
     * MADV_SEQUENTIAL: "I will read front to back, in order."
     *      -> the kernel does aggressive readahead: when i ask for
     *         page 100 it also fetches 101..150 in advance, and it 
     *         drops pages i already passed from the cache sooner.
     *
     * MADV_WILLNEED: "I will need all of it."
     *      -> the kernel starts pulling it in right now, in the
     *         background, instead of waiting for us to fault.
     *
     * This matters a lot here: the data lives on /scratch, which is
     * NFS. Without these hints every page fault is a separate 
     * network round trip. */
    madvise((void *)base, (size_t)st.st_size, MADV_SEQUENTIAL);
    madvise((void *)base, (size_t)st.st_size, MADV_WILLNEED);

    mx->base = base;
    mx->len = (size_t)st.st_size;

    /* Two pointers that bound the region i'm allowed to touch.
     * p is the moving cursor, end never moves. */
    const char *p = base;
    const char *end = base + mx->len;

    /* STEP 5: Check if this is really a Matrix Market file.*/
    
    /* Reject anything too small or wrong before trusting its bytes.
     * The length check must run first and short-circuit: strncmp() below
     * reads 14 bytes starting at p, and without this guard a file under 
     * 14 bytes could make it read past the end of the mapping -> segfaul.*/
    if (mx->len < 15 || strncmp(p, "%%MatrixMarket", 14) != 0) {
        fprintf(stderr, "not a Matrix Market file\n");
        munmap((void *)base, mx->len);      // Undo the mmap before leaving.
        return -1;
    }

    /* STEP 6: Locate the end of the first line.*/

    /* memchr searches for a byte inside a block of memory and returns
     * a pointer to the first occurrence, or NULL if there is none.
     * (Unlike strchr, it does not care about '\0' -- exactly what i
     * need on an mmap.) 
     *
     *  %%MatrixMarket matrix coordinate pattern symmetric\n% test...
     *                                                     ^
     *                                                     nl
     *
     * I need this for two things: the banner's lenght (nl - p), and
     * where to continue afterwards (nl + 1). */
    const char *nl = memchr(p, '\n', mx->len);
    if (!nl) {
        munmap((void *)base, mx->len);
        return -1;
    }

    /* STEP 7: copy the banner out and lowercase it. */

    /* I want case-insensitive searching, but i cannot lowercase the 
     * mapping in place: it is read-only, and its pages are shared
     * with the kernel's page cache. So i copy the line into a small
     * local buffer i own. */
    size_t blen = (size_t)(nl - p);     // Lenght of the first line.
    if (blen > 255) blen = 255;

    char banner[256];
    memcpy(banner, p, blen);
    banner[blen] = '\0';    // Now it is a proper C string.

    for (size_t i = 0; i < blen; i++) {
        banner[i] = (char)tolower((unsigned char)banner[i]);
    }

    /* STEP 8: Decode the banner. */

    /* A banner has five fields. strstr(haystack, needle) returns
     * a pointer to the first place the needle appears inside the
     * haystack, or NULL. I only care whether is is NULL or not. */
    
    /* "coordinate" = a list of "i j" entries (sparse).
     * The alternative, "array", is a dense matrix. I only do sparse. */
    if (!strstr(banner, "coordinate")) {
       fprintf(stderr, "only coordinate format supported\n");
       munmap((void *)base, mx->len);
       return -1;
    }

    /* Complex values would put two number in the value column and 
     * break the line parsing later. */
    if (strstr(banner, "complex")) {
        fprintf(stderr, "complex values not supported\n");
        munmap((void *)base, mx->len);
        return -1;
    }

    /* "pattern" means there is no value column at all, just "i j"
     * Otherwise each line is "i j value" and I must skip the value. */
    mx->is_pattern = (strstr(banner, "pattern") != NULL);

    /* "Symmetric" means only ONE triangle of the matrix is stored, so
     * a stored entry (i, j) also implies (j, i) and i must insert both
     * directions later.
     * 
     * Note "skew-symmetric" contains "symmetric" as a substring. That
     * is fine: it also stores on triangle, and i only care about
     * structure, never about the values. Same for "hermitian". */
    mx->is_symmetric = (strstr(banner, "symmetric") != NULL) ||
                       (strstr(banner, "hermitian") != NULL);
    
    /* STEP 9: Walk past the comment lines. */
     
    /* After the banner there can be zero, one, or fifty comment
     * lines starting with '%', plus possibly blank lines. I cannot
     * know in advance, so i loop until i see something else.
     *
     * '\r' is handled because files written on Windows end lines
     * with "\r\n" rathr than just "\n". */
    p = nl + 1;
    while (p < end && (*p == '%' || *p == '\n' || *p == '\r')) {
        p = skip_line(p, end);
    }
    /* p now points at the dimension line, e.g "4 4 5\n" */

    /* STEP 10: Read the three dimension number. */

    /* The line is "rows columns entries". I parse the digits by hand rather than calling sscanf. */

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

    /* Move past the rest of the dimension line. After this, p points
     * at the first real data line -- the whole point of the file. */
    p = skip_line(p, end);

    /* STEP 11: Sanity checks on the dimensions. */

    /* An adjacency matrix is always square. If it is not, the file
     * is not a graph and nothing below would make sense. */
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

    /* vid_t is uint32_t, so a graph with more than (2^32)-1 vertices
     * cannot be represented. */
    if (dims[0] > VID_MAX) {
        fprintf(stderr, "n = %llu exceeds 32-bit vid_t\n",
                (unsigned long long)dims[0]);
        munmap((void *)base, mx->len);
        return -1;
    }     

    /* STEP 12: record the answers and return. */

    mx->n = (vid_t)dims[0];     /* question 1: how many vertices */
    mx->data = p;               /* question 2: where edgse start */
    /* questions 3 and 4 were answered in step 8 */

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

/* Print what i detected, for the human watching the run.
 *
 * Goes to stderr, not stdout, on purpose: that way the program's 
 * real output can be redirected to a file without diagnostics 
 * getting mixed into it.
 *
 * The %" PRIvid " spelling loooks odd but is just string
 * concantenation. PRIvid expands to "u", so the compiler sees
 * "vertices: %" "u" "\n" and glues it into "vertices: %u\n".
 * The point is that if vid_t ever changes to 64-bit, every printf
 * fixes itself instead of silently printing garbage. */
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
