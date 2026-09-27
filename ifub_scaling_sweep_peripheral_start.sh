#!/bin/bash

# ifub_scaling_sweep_peripheral_start.sh -- same sweep as
# ifub_scaling_sweep.sh, but run against a build of the program where
# two_sweep() returns the PERIPHERAL vertex b instead of the midpoint,
# to compare its effect on the BFS count and the time. Results go to a
# separate CSV file. com-Friendster and NACA0015 have their own scripts.

# --- Slurm resource request (parsed by sbatch before the script runs) ---
#SBATCH --job-name=ifub_sweep_peripheral
#SBATCH --partition=rome        # 128-core AMD EPYC 7662 nodes
#SBATCH --nodes=1               # OpenMP is shared-memory only
#SBATCH --cpus-per-task=128     # whole node -> 1..128 threads, no oversubscription
#SBATCH --mem=0                 # all memory on the node
#SBATCH --exclusive             # no noisy neighbour jobs during timing
#SBATCH --time=15:00:00         # generous time upper bound
#SBATCH --output=slurm_peripheral_start_%j.out

set -u   

# --- Environment setup ---
# icx + libiomp5 give real work-stealing task scheduling (unlike
# gcc/libgomp), which is the property this project measures.
module load intel-oneapi-compilers/2025.2.0

# Spread threads across both NUMA sockets (more memory bandwidth for
# BFS) and pin each one to a distinct core.
export OMP_PROC_BIND=spread
export OMP_PLACES=cores

# --- Configuration ---
BIN=./ifub              # build beforehand with 'make ifub'
OUTDIR=./results
mkdir -p "$OUTDIR"

MASTER_CSV="$OUTDIR/results_of_smaller_graphs_peripheral_start.csv"

# ">" truncates: every fresh submission starts with a clean results file.
echo "graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,diameter,bfs_count,io_csr_time,diameter_time" > "$MASTER_CSV"

# Most threads first: if the job's time limit is hit, only the
# slowest, least informative runs are lost.
THREAD_COUNTS="128 64 32 16 8 4 2 1"

# Graphs to test (all single connected components), one per structure type:
#   com-Amazon              -> product co-purchase network
#   luxembourg_osm          -> road network
#   m14b                    -> 3D finite-element mesh
#   Spielman_k100           -> near-tree (graph Laplacian)
#   preferentialAttachment  -> synthetic scale-free graph
GRAPHS=(
    "/scratch/d/dimopoul/graphs/com-Amazon/com-Amazon.mtx"
    "/scratch/d/dimopoul/graphs/luxembourg_osm/luxembourg_osm.mtx"
    "/scratch/d/dimopoul/graphs/m14b/m14b.mtx"
    "/scratch/d/dimopoul/graphs/Spielman_k100/Spielman_k100.mtx"
    "/scratch/d/dimopoul/graphs/preferentialAttachment/preferentialAttachment.mtx"
)

# --- Main sweep: one run per (graph, thread count) pair ---
for graph in "${GRAPHS[@]}"; do
    name=$(basename "$graph" .mtx)
    echo "=== $name ==="

    for t in $THREAD_COUNTS; do
        echo "  threads=$t"
        export OMP_NUM_THREADS=$t

        # -v prints [debug]/[progress] diagnostics to stderr (ends up in
        # the Slurm output file) without changing stdout, so the
        # "CSV,..." line grep/sed extract below is unaffected.
        "$BIN" -v "$graph" | grep '^CSV,' | sed 's/^CSV,//' >> "$MASTER_CSV"
    done
done

echo "Done. Master CSV: $MASTER_CSV"