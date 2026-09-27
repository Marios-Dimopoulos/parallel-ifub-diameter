# parallel-ifub-diameter

Exact diameter computation for large undirected graphs using the **iFUB**
(iterative Fringe Upper Bound) algorithm, with its parallel fringe search
implemented in C using **OpenMP tasks**. Built for graphs from the
[SuiteSparse Matrix Collection](https://sparse.tamu.edu/), from a few
hundred thousand vertices up to `com-Friendster` (65.6M vertices, 3.6
billion directed entries).

Developed as coursework for the *Parallel and Distributed Systems* course
at the Aristotle University of Thessaloniki, and benchmarked on the
Aristotle HPC cluster.

## Contents

- [Overview](#overview)
- [Algorithm](#algorithm)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Usage](#usage)
- [Verifying correctness](#verifying-correctness)
- [Running the experiments](#running-the-experiments)
- [Results](#results)
- [Known limitations](#known-limitations)

## Overview

The **diameter** of a graph is the longest shortest path between any two
vertices. Computing it exactly by brute force takes one BFS per vertex,
`O(n·(n+m))` — far too slow for graphs with tens of millions of vertices.
**iFUB** computes the *exact* diameter with far fewer BFS calls on most
real-world graphs, by maintaining a lower and an upper bound that converge
as vertices are examined, and stopping as soon as they meet.

This project:
- parses Matrix Market (`.mtx`) files via `mmap`, without copying the file
  into a buffer, and builds a two-pass Compressed Sparse Row (CSR)
  representation.
- picks a good starting vertex for iFUB with a double-sweep heuristic.
- runs the iFUB fringe search with its independent per-level BFS calls
  distributed across an OpenMP task pool, so idle threads steal work from
  busy ones instead of sitting idle.
- includes an independent, embarrassingly-parallel brute-force checker to
  cross-validate the exact-diameter result on graphs small enough to
  afford it.

## Algorithm

```
mtx_open()            memory-map the .mtx file, parse the header only
csr_build_from_mtx()  two-pass build of row_ptr[] / col_idx[] (CSR)
two_sweep()           double sweep -> starting vertex u, lower bound lb0
ifub_diameter()       parallel iFUB fringe search -> exact diameter
```

**`two_sweep()`** runs a BFS from an arbitrary vertex, then a second BFS
from the vertex it found farthest away. The eccentricity reached by the
second BFS is a strong lower bound on the diameter. The vertex returned as
iFUB's starting point `u` is the **midpoint** of that path, not its
endpoint: `iFUB` must examine every vertex farther than roughly `D/2` from
`u`, so a central `u` (`ecc(u) ≈ D/2`) needs far fewer BFS calls than a
peripheral one (`ecc(u) ≈ D`). Correctness holds for either choice — see
[Results](#results) for the measured effect on BFS count.

**`ifub_diameter()`** runs one BFS from `u` to partition every vertex into
levels by distance, then walks the levels from farthest to nearest. For
every vertex in the current level, an OpenMP task runs a BFS to compute
its eccentricity and folds the result into a lock-free atomic lower bound
`lb`. After each level, the upper bound `ub` tightens by the triangle
inequality; the search stops the moment `lb ≥ ub`, at which point `lb` is
proven equal to the diameter. Each thread keeps a private scratch buffer
so its BFS calls never race with another thread's.

## Repository layout

```
include/    Public headers (one per module)
src/        Implementation (mtx, csr, bfs, two-sweep, ifub, brute force, main)
results/    Raw CSV output from every experiment reported on
Makefile    Build rules for ifub and brute_force_bfs
*.sh        Slurm batch scripts for each experiment (see below)
```

| Module | Responsibility |
|---|---|
| `mtx.c` / `mtx.h` | Memory-maps a `.mtx` file and parses only its header (dimensions, symmetry, pattern vs. weighted) |
| `csr.c` / `csr.h` | Two-pass construction of the CSR graph from a parsed `.mtx` |
| `bfs_eccentricity.c` / `.h` | Single-source BFS; returns the eccentricity of the source vertex |
| `two_sweep.c` / `.h` | Double-sweep heuristic for iFUB's starting vertex and initial lower bound |
| `ifub.c` / `.h` | The parallel iFUB fringe search (OpenMP tasks) |
| `brute_force_bfs.c` / `.h` | Independent, parallel BFS-from-every-vertex diameter check |
| `main.c` | CLI entry point: wires the pipeline together, times it, prints the report |

## Building

Requires a C11 compiler with OpenMP task support. Developed with Intel's
`icx` (oneAPI) for its work-stealing `libiomp5` runtime; it also builds
cleanly with GCC and its `libgomp` runtime.

```bash
make ifub              # the diameter tool
make brute_force_bfs   # the independent correctness checker
make clean
```

`CFLAGS` in the `Makefile` includes `-march=znver2`, tuned for the AMD
EPYC 7662 nodes this project was benchmarked on. **Remove or change that
flag** if building for a different CPU architecture.

## Usage

```bash
./ifub [-v] <file.mtx>
```

Prints a human-readable report to stdout and one machine-readable
`CSV,...` line (used by the sweep scripts below). `-v` additionally prints
`[debug]`/`[progress]` diagnostics to stderr — the current BFS count, and
the lower/upper bound as they tighten — without affecting the result.

```text
$ OMP_NUM_THREADS=128 ./ifub -v /path/to/com-Amazon.mtx
[debug] initial BFS done, h=47
...
diameter               : 47
total BFS calls        : 253
diameter time (s)      : 0.136
CSV,/path/to/com-Amazon.mtx,128,334863,1851744,291710,47,47,253,0.065,0.136
```

The graph must be a single connected component (checked explicitly after
the first BFS) and undirected. A `symmetric` Matrix Market banner is
handled by inserting each stored edge in both directions; a `general`
banner is trusted to already list both directions of every edge.

## Verifying correctness

`brute_force_bfs` computes the diameter by running a BFS from *every*
vertex (parallelized the same way as iFUB's fringe search) and, given an
expected value, reports a match or mismatch:

```bash
OMP_NUM_THREADS=128 ./brute_force_bfs <file.mtx> [expected_diameter]
```

Exit codes: `0` success (and match, if an expected value was given), `1`
error (not connected, I/O failure, out of memory), `2` mismatch.

This was used to cross-check `ifub`'s result on every graph small enough
to afford `O(n·(n+m))` work (confirmed match on, for example,
`com-Amazon`, diameter 47). `com-Friendster` and `NACA0015` are too large
to brute-force in reasonable time and are not independently re-verified
this way; their correctness relies on the algorithm's proof and on the
smaller-graph cross-checks.

## Running the experiments

> **The graph paths in these scripts are hard-coded to the author's own
> `/scratch` directory on the Aristotle HPC cluster** (e.g.
> `/scratch/d/dimopoul/graphs/com-Amazon/com-Amazon.mtx`). They are
> committed as-run, for reproducibility of the exact commands behind the
> results below — **not as a portable configuration**. To run these
> scripts yourself, download the corresponding `.mtx` files (e.g. from the
> [SuiteSparse Matrix Collection](https://sparse.tamu.edu/)) and edit the
> `GRAPHS=(...)` array near the top of each script to point at wherever
> you placed them. The `#SBATCH --partition=rome` line is likewise
> specific to this cluster and will need to match a partition that exists
> on yours.

Four Slurm batch scripts, one per experiment, all writing to `results/`:

| Script | Graphs | Purpose |
|---|---|---|
| `ifub_scaling_sweep.sh` | 5 light graphs (`com-Amazon`, `luxembourg_osm`, `m14b`, `Spielman_k100`, `preferentialAttachment`) | Thread-scaling sweep, `OMP_NUM_THREADS` = 128 down to 1 |
| `ifub_scaling_sweep_naca0015.sh` | `NACA0015` | Same sweep, isolated (≈204K BFS calls per run, hours at low thread counts) |
| `ifub_scaling_sweep_com_friendster.sh` | `com-Friendster` | Same sweep, isolated (65.6M vertices, per-run `timeout`) |
| `ifub_scaling_sweep_peripheral_start.sh` | The 5 light graphs again | Same sweep, built against a variant of `two_sweep()` that returns the *peripheral* vertex instead of the midpoint — isolates the effect of the starting-vertex choice |

The peripheral-start variant is not shipped as a build option: reproduce
it by temporarily changing `two_sweep()`'s final `return cur;` to
`return b;` in `src/two_sweep.c`, rebuilding, running that script, then
reverting.

All four scripts assume the graphs already exist locally as `.mtx` files
(paths at the top of each script) and that `make ifub` has already been
run. Each writes a CSV with columns:

```
graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,diameter,bfs_count,io_csr_time,diameter_time
```

## Results

One thread (`T1`) and 128 threads (`T128`) diameter-computation time,
`bfs_count` = BFS calls in iFUB's fringe loop (excludes the 3 sequential
BFS before it: two from `two_sweep`, one to build the levels):

| Graph | `n` | `m` | Diameter | `bfs_count` | `T1` (s) | `T128` (s) | Speedup |
|---|---:|---:|---:|---:|---:|---:|---:|
| preferentialAttachment | 100,000 | 999,970 | 7 | 4,126 | 13.6 | 0.15 | 92.8× |
| luxembourg_osm | 114,599 | 239,332 | 1,337 | 1 | 0.010 | 0.021 | 0.5× |
| m14b | 214,765 | 3,358,036 | 51 | 77 | 1.13 | 0.078 | 14.4× |
| com-Amazon | 334,863 | 1,851,744 | 47 | 253 | 3.19 | 0.14 | 23.5× |
| Spielman_k100 | 338,402 | 687,002 | 101 | 169,201 | 303.9 | 4.80 | 63.3× |
| NACA0015 | 1,039,183 | 6,229,636 | 1,267 | 204,361 | 11,102.1 | 149.8 | 74.1× |
| com-Friendster | 65,608,366 | 3,612,134,270 | 37 | 14 | 455.2 | 188.4 | 2.4× |

**Speedup tracks the amount of independent work, not the graph's size.**
Graphs whose fringe search needs only a handful of BFS calls
(`luxembourg_osm`, `com-Friendster`) see little or no benefit from more
threads — `com-Friendster` at 128 threads is *slower* than at 64,
consistent with per-level OpenMP team/task overhead dominating once there
is too little work to hide it behind. Graphs needing thousands of BFS
calls (`preferentialAttachment`, `Spielman_k100`, `NACA0015`) scale well.

**The starting-vertex heuristic matters more than the thread count.**
Rerunning the 5 light graphs with `two_sweep()` returning the peripheral
vertex instead of the midpoint inflates `bfs_count` dramatically, with the
*same* diameter result in every case:

| Graph | `bfs_count` (midpoint) | `bfs_count` (peripheral) |
|---|---:|---:|
| luxembourg_osm | 1 | 47,790 |
| m14b | 77 | 182,633 |
| com-Amazon | 253 | 265,586 |
| preferentialAttachment | 4,126 | 98,934 |
| Spielman_k100 | 169,201 | 338,301 |

A peripheral starting vertex makes iFUB degenerate towards brute force on
several of these graphs, confirming that the midpoint heuristic is not a
cosmetic choice: it is what makes iFUB worth using in the first place on
graphs where the naive double-sweep would otherwise land on an endpoint.

## Known limitations

- If a fringe-search BFS's internal allocation fails under memory
  pressure, its result is silently dropped rather than aborting the run
  — a risk only at extreme graph sizes.
- A blank line inside a `.mtx` file's data section is treated as a
  malformed entry.
- `general`-format (non-symmetric-banner) matrices are trusted to already
  list every edge in both directions; this is not independently verified.
- CSR construction is single-threaded, so under Linux's default
  first-touch NUMA policy the graph's memory ends up local to a single
  socket while the fringe search's threads span both — a likely
  contributor to the sub-linear scaling reported above on multi-socket
  runs.