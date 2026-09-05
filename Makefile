CC ?= icx
CFLAGS = -O3 -g -march=znver2 -fiopenmp -Wall -Wextra -Iinclude
LDFLAGS = -fiopenmp

test_ifub: src/mtx.c src/csr.c src/bfs_eccentricity.c src/two_sweep.c src/ifub.c src/test_ifub.c \
               include/types.h include/mtx.h include/csr.h include/bfs_eccentricity.h include/two_sweep.h include/ifub.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/csr.c src/bfs_eccentricity.c src/two_sweep.c src/ifub.c src/test_ifub.c $(LDFLAGS)

clean:
	rm -f test_ifub