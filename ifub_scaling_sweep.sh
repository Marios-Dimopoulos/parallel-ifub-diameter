#!/bin/bash

# ifub_scaling_sweep.sh -- runs the iFUB diameter program across
# multiple graphs and mulitple OMP_NUM_THREADS values, and
# collects every run's results as one row in a single CSV file.
#
# No per-run log files are kept: each CSV already
# carries every piece of information i care
# about (graph name, thread count, vertex/edge counts, two-sweep
# results, diameter, BFS count, and both timing phases), so a 
# separate human-readable log would just be a redundant copy of 
# the same data in harder-to-parse format.
# 
# com-Friendster is deliberately NOT included in this sweep: at 
# 65.6M vertices it needs its own job with a much larger --time
# budget.

# --- Slurm resource request directives ---
# These lines are not shell comments -- sbatch parses any
# line starting with "#SBATCH" out of this file before the script
# ever runs, and turns them into the actual resource request. They
# must appear before the first real shel command in the file, or 
# Slurm stops reading them.
#SBATCH --job-name=ifub_sweep_main
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
#SBATCH --time=12:00:00         # generous time upper bound.
#SBATCH --output=slurm_%j.out   # %j is substituted by Slurm with this 
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
module load intel-openapi-compilers/2025.2.0

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

# Required for the cooperative early-stop logic inside ifub_diameter():
# every task checks a shared stop_flag before starting its own BFS, 
# and that flag is only meaningful once OpenMP's cancellation
# machinery is switched on cluster-wide via this enviroment variable.
export OMP_CANCELLATION=true

# --- Configuration ---
BIN=./ifub              # The compiled binary, expected to already
                        # exist in the current working directory
                        # (build beforehand with 'make ifub')
OUTDIR=./results
mkdir -p "$OUTDIR"      # -p: create directories as needed
                        # and do NOT fail if the directory already
                        # exists from a previous run of this script.

MASTER_CSV="$OUTDIR/all_results.csv"

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

# Graphs to test, orderd from EASIEST to HARDEST for iFUB to
# converge on, based on prior exploratory runs:
#   delaunay_n14      -> mesh/geometric, already confirmed hard,
#                        but small and fast (seconds, not minutes)
#   coPapersCiteseer  -> citation/collab, HYPOTHESISED easy (not yet
#                        confirmed -- this sweep is what tests that)
#   com-DBLP          -> social/collab, already confirmed hard
#                        (99% of vertices needed a BFS call)
#   roadNet-PA        -> road network, expected hard by analogy with
#                        naca0015/delaunay (same "large diameter,
#                        narrow fringe levels" structural profile)
#   naca0015          -> mesh/geometric, already confirmed hard AND
#                        slow -- placed last on purpose
GRAPHS=(
    "/scratch/d/dimopoul/graphs/delaunay_n14/delaunay_n14.mtx"
    "/scratch/d/dimopoul/graphs/coPapersCiteseer/coPapersCiteseer.mtx"
    "/scratch/d/dimopoul/graphs/com-DBLP/com-DBLP.mtx"
    "/scratch/d/dimopoul/graphs/roadNet-PA/roadNet-PA.mtx"
    "/scratch/d/dimopoul/graphs/NACA0015/NACA0015.mtx"
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

        # Run the program exactly once. Its normal (non -v) stdout
        # includes exactly one line starting with the literal text
        # "CSV,", holding every field we need, already
        # comma-separated in the same column order as the header
        # written above. The pipeline below:
        #   1. "2>/dev/null"      discards stderr entirely (harmless
        #                         here since -v/verbose mode, which
        #                         is the only thing that writes to
        #                         stderr, is never enabled by this
        #                         script)
        #   2. "grep '^CSV,'"     keeps ONLY the one line that starts
        #                         with "CSV," out of the ~10 lines
        #                         the program actually prints
        #   3. "sed 's/^CSV,//'"  strips that leading "CSV," prefix,
        #                         since the master file's header row
        #                         (written once, above) already
        #                         labels every column -- repeating
        #                         the literal word "CSV" on every
        #                         single data row would be redundant
        #   4. ">> $MASTER_CSV"   appends (not overwrites) the
        #                         resulting single line as one new
        #                         row of the growing master file
        "$BIN" "$graph" 2>/dev/null | grep '^CSV,' | sed 's/^CSV,//' >> "$MASTER_CSV"
    done
done

echo "Done. Master CSV: $MASTER_CSV"