CC ?= gcc
CFLAGS = -O0 -g -march=znver2 -fopenmp -Wall -Wextra -Iinclude
LDFLAGS = -fopenmp

test_mtx: src/mtx.c src/test_mtx.c include/csr.h include/mtx.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/test_mtx.c $(LDFLAGS)

test_csr: src/mtx.c src/csr.c src/test_csr.c include/csr.h include/mtx.h include/types.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/csr.c src/test_csr.c $(LDFLAGS)

test_bfs_eccentricity: src/mtx.c src/csr.c src/bfs_eccentricity.c src/test_bfs_eccentricity.c include/types.h include/mtx.h include/csr.h include/bfs_eccentricity.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/csr.c src/bfs_eccentricity.c src/test_bfs_eccentricity.c $(LDFLAGS)

test_two_sweep: src/mtx.c src/csr.c src/bfs_eccentricity.c src/two_sweep.c src/test_two_sweep.c \
                include/types.h include/mtx.h include/csr.h include/bfs_eccentricity.h include/two_sweep.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/csr.c src/bfs_eccentricity.c src/two_sweep.c src/test_two_sweep.c $(LDFLAGS)

clean:
	rm -f test_mtx test_csr test_bfs_eccentricity test_two_sweep