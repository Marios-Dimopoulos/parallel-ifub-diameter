#!/bin/bash

# ifub_scaling_sweep_com_friendster.sh -- same sweep as
# ifub_scaling_sweep.sh, for com-Friendster alone (65.6M vertices,
# needs its own --time budget and a per-run timeout). It needs only a
# handful of BFS calls, so it is expected to show that more threads do
# not always mean a higher speedup.

# --- Slurm resource request (parsed by sbatch before the script runs) ---
#SBATCH --job-name=ifub_sweep_friendster
#SBATCH --partition=rome        # 128-core AMD EPYC 7662 nodes
#SBATCH --nodes=1               # OpenMP is shared-memory only
#SBATCH --cpus-per-task=128     # whole node -> 1..128 threads, no oversubscription
#SBATCH --mem=0                 # all memory on the node (~80 GB estimated peak)
#SBATCH --exclusive             # no noisy neighbour jobs during timing
#SBATCH --time=12:00:00         # larger budget: graph size
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

MASTER_CSV="$OUTDIR/results_of_com_friendster.csv"

# ">" truncates: every fresh submission starts with a clean results file.
echo "graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,diameter,bfs_count,io_csr_time,diameter_time" > "$MASTER_CSV"

# Most threads first, as in ifub_scaling_sweep.sh. Every run re-reads
# the ~30 GB file (~8 minutes) before the diameter computation starts.
THREAD_COUNTS="128 64 32 16 8 4 2 1"

# Stop each run 20 min before the Slurm --time limit above (700m =
# 11h40m), so the job prints a message instead of being killed.
RUN_TIMEOUT="700m"

GRAPHS=(
    "/scratch/d/dimopoul/graphs/com-Friendster/com-Friendster.mtx"
)

# --- Main sweep: one run per (graph, thread count) pair ---
for graph in "${GRAPHS[@]}"; do
    name=$(basename "$graph" .mtx)
    echo "=== $name ==="

    for t in $THREAD_COUNTS; do
        echo "  threads=$t"
        export OMP_NUM_THREADS=$t

        # -v prints [debug]/[progress] diagnostics to stderr (Slurm's
        # output file) without changing stdout, so grep/sed still
        # extract the "CSV,..." line below. The run is wrapped in
        # `timeout`; PIPESTATUS[0] (not $?, which would be sed's exit
        # code) reports 124 for a timeout or any other non-zero code
        # for a failure -- either way no CSV row is written.
        timeout "$RUN_TIMEOUT" "$BIN" -v "$graph" | grep '^CSV,' | sed 's/^CSV,//' >> "$MASTER_CSV"
        status=${PIPESTATUS[0]}
        if [ "$status" -eq 124 ]; then
            echo "  TIMEOUT after $RUN_TIMEOUT: no result for threads=$t"
        elif [ "$status" -ne 0 ]; then
            echo "  FAILED with exit code $status: no result for threads=$t"
        fi
    done
done

echo "Done. Master CSV: $MASTER_CSV"