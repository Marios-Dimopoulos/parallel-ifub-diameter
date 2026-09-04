#include <stdio.h>
#include <stdlib.h>
#include "mtx.h"
#include "csr.h"
#include "bfs_eccentricity.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <file.mtx> <source-vertex-1-based>\n", argv[0]);
        return 1;
    }

    mtx_t mx;
    if (mtx_open(argv[1], &mx) != 0) {
        return 1;
    }
    mtx_describe(&mx);

    csr_t g;
    if (csr_build_from_mtx(&mx, mtx_policy(&mx), &g) != 0) {
        mtx_close(&mx);
        return 1;
    }
    mtx_close(&mx);     // CSR is self-contained now, the .mtx mapping is no longer needed.

    vid_t source = (vid_t)(atoi(argv[2]) - 1);      // Accept 1-based like the file format.
    if (source >= g.n) {
        fprintf(stderr, "srouce our of scope: n = %" PRIvid "\n", g.n);
        csr_free(&g);
        return 1;
    }

    dist_t *dist = malloc((size_t)g.n * sizeof(dist_t));
    dist_t ecc = bfs_eccentricity(&g, source , dist);

    vid_t reached = 0;
    for (vid_t v = 0; v < g.n; v++) {
        if (dist[v] != DIST_UNREACHED) {
            reached++;
        }
    }

    printf("source (0-based): %" PRIvid "\n", source);
    printf("eccentricity: %" PRIdist "\n", ecc);
    printf("reached: %" PRIvid " / %" PRIvid "\n", reached, g.n);

    /* Print the firts handful of distances so i can eyeball them
     * against the graph by hand on a small test file. */
     printf("first distances: ");
     for (vid_t v = 0; v < g.n && v < 20; v++) {
        printf("%" PRIdist " ", dist[v]);
     }
     printf("\n");

     free(dist);
     csr_free(&g);
     return 0;
}