#!/bin/bash

# ifub_scaling_sweep.sh -- runs the iFUB diameter program across
# multiple graphs and miltiple OMP_NUM_THREADS values, and
# collects every run's result as one row in a single CSV file.
# 
# No per-run log files are kept: the CSV line already carries
# every piece of information the program prints (graph name,
# thread count, vertex/edge counts, two-sweep results, diameter,
# BFS count, and bot timing phases), so a separate human-
# readable log would just be a redundant copy of the same data.

#SBATCH --job-name=ifub_scaling
#SBATCH --partition=rome
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --mem=0
#SBATCH --exclusive
#SBATCH --time=12:00:00
#SBATCH --output=slurm_%j.out

# I deliberately do NOT use "set -e" here. If a single run fails
# for any reason (a crash, an out-of-memoy condition, or the 
# overall Slurm job time limint being hit mid-run), "set -e" would
# abort the ENTIRE script immediately -- discarding every result
# already collected from earlier, successfull runs. Without it, a 
# failed run just poduces and incomplete/missing line, and the
# script moves on to the next configuration.
set -u      # still treat the use of an undefined shell variable
            # (e.g. a typo in a variable name) as an error, since
            # that kind of mistake should never be silently ignored.

# Enviroment setup.
# icx is Clang-based and links against Intel-s OpenMP runtime
# (libomp5), which -- unlike GCC's libgomp -- implements real
# work-stealing task sheduling.
module load intel-oneapi-compilers/2025.2.0

# OMP_PROC_BIND=spread encourages the runtime to spread threads
# across both NUMA sockets rather than packing them onto one,
# which matters for a memory-bandwidth-bound workload like BFS.
export OMP_PROC_BIND=spread
export OMP_PLACES=cores
export OMP_CANCELLATION=true    # Requiered for the cooperative
                                # early-stop logig inside iFUB

# Configuration
BIN=./ifub
OUTDIR=./results
mkdir -p "$OUTDIR"

MASTER_CSV="$OUTDIR/all_results.csv"
echo "graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,diameter,bfs_count,io_csr_time,diameter_time" > "$MASTER_CSV"

# Thread counts to sweep, from MOST threads down to FEWEST. This
# ordering matters: a run with few threads on a hard graph can take
# far longer than the same graph with many threads. Running the
# large-thread-cound cases first means that if the job's time limit
# is reached before the sweep finishes, i lose only the slowest,
# least time-efficient data points -- not the more informative ones.
THREAD_COUNTS="128 64 32 16 8 4 2 1"

# Graphs to test, orderd from EASIEST to HARDEST for iFUB to 
# converge on, based on prior exploratory runs:
#   delaunay_n14, naca0015  -> mesh/geometric, known to be hard
#   com-DBLP                -> social/collab, known to be hard
#   coPapersCiteseer        -> citation/collab, hypothesised easy
#   roadNet-PA              -> road network, expected hard
# Placing the graphs i already know are expensive later in the 
# list means a time-limit cutoff loses the least-informative
# already-confirmed-slow runs first.
GRAPHS=(
    "/scratch/d/dimopoul/graphs/delaunay_n14/delaunay_n14.mtx"
    "/scratch/d/dimopoul/graphs/coPapersCiteseer/coPapersCiteseer.mtx"
    "/scratch/d/dimopoul/graphs/com-DBLP/com-DBLP.mtx"
    "/scratch/d/dimopoul/graphs/roadNet-PA/roadNet-PA.mtx"
    "/scratch/d/dimopoul/graphs/naca0015/naca0015.mtx"
)

# Main sweep
for graph in "${GRAPHS[@]}"; do
    name=$(basename "$graph" .mtx)
    echo "=== $name ==="

    for t in $THREAD_COUNTS; do
        echo "  threads=$t"
        export OMP_NUM_THREADS=$t

       # Run the program once, Its normal (non -v) stdout included
       # one line starting with "CSV,", holding every field i need, 
       # already comma-separated. I filter for just that
       # line, strip the "CSV," prefix (since the master file
       # already has its own header), and append the result as one 
       # row of the master CSV. Nothing ees from the run's output 
       # is kept anywhere.
       "$BIN" "$graph" 2>/dev/null | grep '^CSV,' | sed 's/^CSV,//' >> "$MASTER_CSV"
    done
done

echo "Done. Master CSV: $MASTER_CSV"