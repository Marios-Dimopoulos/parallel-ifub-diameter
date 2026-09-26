#!/bin/bash

# ifub_scaling_sweep_peripheral_start.sh -- runs the iFUB diameter program across
# multiple graphs and mulitple OMP_NUM_THREADS values, and
# collects every run's results as one row in a single CSV file.
#
# This is the SAME sweep as ifub_scaling_sweep.sh, run with a build of the
# program in which two_sweep() returns the PERIPHERAL vertex b instead of the
# midpoint of the a-b path. The results go to a separate CSV file, to compare
# the effect of the starting vertex on the BFS count and on the time.
#
# No per-run log files are kept: each CSV already
# carries every piece of information i care
# about (graph name, thread count, vertex/edge counts, two-sweep
# results, diameter, BFS count, and both timing phases), so a 
# separate human-readable log would just be a redundant copy of 
# the same data in harder-to-parse format.
# 
# com-Friendster and NACA0015 are deliberately NOT included in this sweep:
# they need their own jobs with a much larger --time budget (see
# ifub_scaling_sweep_com_friendster.sh and ifub_scaling_sweep_naca0015.sh).

# --- Slurm resource request directives ---
# These lines are not shell comments -- sbatch parses any
# line starting with "#SBATCH" out of this file before the script
# ever runs, and turns them into the actual resource request. They
# must appear before the first real shel command in the file, or 
# Slurm stops reading them.

# --- Slurm resource request directives ---
#SBATCH --job-name=ifub_sweep_peripheral
#SBATCH --partition=rome        # The only partition with the 128-core
                                # AMD EPYC 7662 nodes i benchmarked on.
#SBATCH --nodes=1               # OpenMP is shared-memory only -- there 
                                # is no benefit from requesting more than
                                # one physical node.
#SBATCH --cpus-per-task=128     # reserve every physical core of the 
                                # node for this one task, so OMP_NUM_THREADS
                                # can be safely set anywhere from 1 to 128
                                # without oversubscribing
#SBATCH --mem=0                 # special Slurm value meaning "give this
                                # job all memory available on the node",
                                # rather than typing an explicit GB figure.
#SBATCH --exclusive             # Do not let Slurm schedule anything else onto 
                                # this same node while ours runs -- essential for
                                # trustworthy timing measurements, since
                                # BFS is memory-bandwidth-bound and a
                                # noisy neighbour job would silently
                                # ruin every result.
#SBATCH --time=15:00:00         # generous time upper bound.
#SBATCH --output=slurm_peripheral_start_%j.out   # %j is substituted by Slurm with this 
                                # job's numeric ID, so repeated
                                # submissions never overwrite each
                                # other's console output.

set -u   

# --- Enviroment setup ---
#
# icx is Clang-based and links against Intel's OpenMP runtime
# (libiomp5.so), which -- unlike GCC's libgomp -- implements real
# work-stealing task scheduling with per-thread work queue. This is the
# property the assignmenet specifically asks me to use and 
# measure, so icx/libiomp5 was chosen deliberately over gcc/libgomp.
module load intel-oneapi-compilers/2025.2.0

# OMP_PROC_BIND=spread encourages the runtime to spread threads
# across BOTH NUMA sockets of the node rather than packing them onto
# one socket first. This matters specifically because BFS is 
# memory-bandwidth-bound: spreading threads means both sockets
# memory controllers are used simultaneously, roughly doubling
# available bandwidth compared to saturating on socket alone.
export OMP_PROC_BIND=spread

# OMP_PLACES=cores pins each OpenMP thread to a distinct physical
# core, preventing the OS scheduler from migrating threads between
# cores mid-run (which would hurt cache locality) or from placing
# two threads on the same physical core.
export OMP_PLACES=cores


# --- Configuration ---
BIN=./ifub              # The compiled binary, expected to already
                        # exist in the current working directory
                        # (build beforehand with 'make ifub')
OUTDIR=./results
mkdir -p "$OUTDIR"      # -p: create directories as needed
                        # and do NOT fail if the directory already
                        # exists from a previous run of this script.

MASTER_CSV="$OUTDIR/results_of_smaller_graphs_peripheral_start.csv"

# Write the CSV header once, before any run happens. Using a plain
# ">" (truncate/overwrite) here, not ">>", is intentional: every
# fresh submission of this script should start a brand new results
# file rather than silently appending to stale data left over from
# a previous, possible different, experiment.
echo "graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,diameter,bfs_count,io_csr_time,diameter_time" > "$MASTER_CSV"

# Thread counts to sweep, listed from MOST threads down to FEWEST.
# This ordering is a deliberate choice, not arbitrary: a run with
# few threads on a hard graph can take far longer than the same
# graph with many threads. Running the large-thread-count cases FIRST
# means that if the job's time limit is ever reached before 
# the full sweep finishes, i only lose the slowest, least time-efficient 
# data points at the tail end -- not the faster, more numerous, more 
# informative ones.
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

# --- Main sweep ---
#
# Nested loop: outer over graphs, inner ove thread counts. For
# each (graph, thread_count) pair, the program runs exactly ONCE --
# there is no separate "log run" and "CSV run"; the same single
# invocation's output is filtered directly into the master CSV.
for graph in "${GRAPHS[@]}"; do
    # basename strips the directory path, and the ".mtx" suffix
    # removal via the second argument gives us just the bare graph
    # name (e.g. "naca0015") for the human-readable progress message
    # below -- it has no effect on the CSV contents themselves,
    # since the program prints the full original path anyway.
    name=$(basename "$graph" .mtx)
    echo "=== $name ==="

    for t in $THREAD_COUNTS; do
        echo "  threads=$t"

        # OMP_NUM_THREADS is read by the OpenMP runtime at the
        # moment "$BIN" starts, not before -- so re-exporting it
        # here, once per inner-loop iteration, correctly changes the
        # thread count used by the NEXT invocation of the program.
        export OMP_NUM_THREADS=$t

        # Run the program exactly once. Its stdout includes exactly one line
        # starting with the literal text "CSV,", holding every field we
        # need, already comma-separated in the same column order as the
        # header written above. The pipeline below:
        #   1. "grep '^CSV,'"     keeps ONLY that line out of the ~10 lines
        #                         the program prints
        #   2. "sed 's/^CSV,//'"  strips the "CSV," prefix, since the header
        #                         row (written once, above) already labels
        #                         every column
        #   3. ">> $MASTER_CSV"   appends the line as one new row of the
        #                         master file
        # stderr is not redirected, so any error message ends up in the
        # Slurm output file (slurm_*.out).
        "$BIN" "$graph" | grep '^CSV,' | sed 's/^CSV,//' >> "$MASTER_CSV"
    done
done

echo "Done. Master CSV: $MASTER_CSV"