#!/bin/bash

# ifub_scaling_sweep_naca0015.sh -- same sweep as ifub_scaling_sweep.sh,
# for NACA0015 alone (1.04M vertices, ~204k BFS calls per run: the
# heaviest graph here, so runs with few threads take hours). Needs its
# own --time budget and a job-wide remaining-time check before each run.

# --- Slurm resource request (parsed by sbatch before the script runs) ---
#SBATCH --job-name=ifub_sweep_naca0015
#SBATCH --partition=rome        # 128-core AMD EPYC 7662 nodes
#SBATCH --nodes=1               # OpenMP is shared-memory only
#SBATCH --cpus-per-task=128     # whole node -> 1..128 threads, no oversubscription
#SBATCH --mem=0                 # all memory on the node
#SBATCH --exclusive             # no noisy neighbour jobs during timing
#SBATCH --time=14:00:00         # ~8h estimated + safety margin
#SBATCH --output=slurm_%j.out

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

MASTER_CSV="$OUTDIR/results_of_naca0015.csv"

# ">" truncates: every fresh submission starts with a clean results file.
echo "graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,diameter,bfs_count,io_csr_time,diameter_time" > "$MASTER_CSV"

# Most threads first: if the job's time limit is hit, only the
# slowest, least informative runs are lost.
THREAD_COUNTS="128 64 32 16 8 4 2 1"

# Must match --time above (14:00:00 = 50400s). SAFETY_S is kept free
# so the job finishes on its own instead of being killed by Slurm.
JOB_TIME_LIMIT_S=50400
SAFETY_S=1200           # 20 minutes

GRAPHS=(
    "/scratch/d/dimopoul/graphs/NACA0015/NACA0015.mtx"
)

# --- Main sweep: one run per (graph, thread count) pair ---
for graph in "${GRAPHS[@]}"; do
    name=$(basename "$graph" .mtx)
    echo "=== $name ==="

    for t in $THREAD_COUNTS; do
        echo "  threads=$t"
        export OMP_NUM_THREADS=$t

        # Time left in the job (budget - safety margin - $SECONDS
        # elapsed so far). Skip this run if none is left.
        remaining=$(( JOB_TIME_LIMIT_S - SAFETY_S - SECONDS ))
        if [ "$remaining" -le 0 ]; then
            echo "  SKIPPED: not enough time left in the job for threads=$t"
            continue
        fi

        # -v prints [debug]/[progress] diagnostics to stderr without
        # changing stdout, so grep/sed still extract the "CSV,..."
        # line. PIPESTATUS[0] (not $?, sed's exit code) reports 124
        # for a timeout or any other non-zero code for a failure --
        # either way no CSV row is written.
        timeout "${remaining}s" "$BIN" -v "$graph" | grep '^CSV,' | sed 's/^CSV,//' >> "$MASTER_CSV"
        status=${PIPESTATUS[0]}
        if [ "$status" -eq 124 ]; then
            echo "  TIMEOUT after ${remaining}s: no result for threads=$t"
        elif [ "$status" -ne 0 ]; then
            echo "  FAILED with exit code $status: no result for threads=$t"
        fi
    done
done

echo "Done. Master CSV: $MASTER_CSV"