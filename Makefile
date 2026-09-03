CC ?= gcc
CFLAGS = -O0 -g -march=znver2 -fopenmp -Wall -Wextra -Iinclude
LDFLAGS = -fopenmp

test_mtx: src/mtx.c src/test_mtx.c include/graph.h include/mtx.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/test_mtx.c $(LDFLAGS)

clean:
	rm -f test_mtx mtx2csr