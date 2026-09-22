CC ?= icx
CFLAGS = -O3 -g -march=znver2 -fiopenmp -Wall -Wextra -Iinclude
LDFLAGS = -fiopenmp

ifub: src/mtx.c src/csr.c src/bfs_eccentricity.c src/two_sweep.c src/ifub.c src/main.c \
      include/types.h include/mtx.h include/csr.h include/bfs_eccentricity.h include/two_sweep.h include/ifub.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/csr.c src/bfs_eccentricity.c src/two_sweep.c src/ifub.c src/main.c $(LDFLAGS)

brute_force_bfs: src/mtx.c src/csr.c src/bfs_eccentricity.c src/brute_force_bfs.c \
                 include/types.h include/mtx.h include/csr.h include/bfs_eccentricity.h include/brute_force_bfs.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/csr.c src/bfs_eccentricity.c src/brute_force_bfs.c $(LDFLAGS)

clean:
	rm -f ifub brute_force_bfs