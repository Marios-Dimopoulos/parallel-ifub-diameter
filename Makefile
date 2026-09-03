CC ?= gcc
CFLAGS = -O0 -g -march=znver2 -fopenmp -Wall -Wextra -Iinclude
LDFLAGS = -fopenmp

test_mtx: src/mtx.c src/test_mtx.c include/csr.h include/mtx.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/test_mtx.c $(LDFLAGS)

test_csr: src/mtx.c src/csr.c src/test_csr.c include/csr.h include/mtx.h include/types.h
	$(CC) $(CFLAGS) -o $@ src/mtx.c src/csr.c src/test_csr.c $(LDFLAGS)

clean:
	rm -f test_mtx test_csr 