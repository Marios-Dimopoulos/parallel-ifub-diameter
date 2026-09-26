#!/bin/bash

# ifub_scaling_sweep_com_friendster.sh -- runs the iFUB diameter program
# on com-Friendster only, across the same OMP_NUM_THREADS values as
# ifub_scaling_sweep.sh, and collects every run's result as one row in
# its own CSV file.
#
# com-Friendster (65.6M vertices, 3.6 billion directed entries) is
# deliberately kept out of ifub_scaling_sweep.sh: it needs its own job
# with a much larger --time budget. Apart from the graph list and the
# CSV name, this script is written exactly like ifub_scaling_sweep.sh.
# This graph needs only a handful of BFS calls in the iFUB loop, so the
# sweep is expected to show that more threads do not always mean a
# higher speedup.

# --- Slurm resource request directives ---
# These lines are not shell comments -- sbatch parses any
# line starting with "#SBATCH" out of this file before the script
# ever runs, and turns them into the actual resource request. They
# must appear before the first real shel command in the file, or
# Slurm stops reading them.
#SBATCH --job-name=ifub_sweep_friendster
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
                                # The estimated peak for com-Friendster at
                                # 128 threads is around 80 GB.
#SBATCH --exclusive             # Do not let Slurm schedule anything else onto
                                # this same node while ours runs -- essential for
                                # trustworthy timing measurements, since
                                # BFS is memory-bandwidth-bound and a
                                # noisy neighbour job would silently
                                # ruin every result.
#SBATCH --time=12:00:00         # much larger budget than the main sweep,
                                # because of the size of the graph.
#SBATCH --output=slurm_%j.out   # %j is substituted by Slurm with this
                                # job's numeric ID, so repeated
                                # submissions never overwrite each
                                # other's console output.

set -u

# --- Enviroment setup ---
#
# icx is Clang-based and links against Intel's OpenMP runtime
# (libiomp5.so), so the module that provides it must be loaded here too.
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

MASTER_CSV="$OUTDIR/results_of_com_friendster.csv"

# Write the CSV header once, before any run happens. Using a plain
# ">" (truncate/overwrite) here, not ">>", is intentional: every
# fresh submission of this script should start a brand new results
# file rather than silently appending to stale data left over from
# a previous, possible different, experiment.
echo "graph,threads,vertices,edges,two_sweep_start,two_sweep_lb,diameter,bfs_count,io_csr_time,diameter_time" > "$MASTER_CSV"

# Thread counts to sweep, listed from MOST threads down to FEWEST,
# exactly like ifub_scaling_sweep.sh. Every run reads the ~30 GB file
# again (about 8 minutes) before the diameter computation starts.
THREAD_COUNTS="128 64 32 16 8 4 2 1"

# Stop EACH run 20 minutes before the Slurm limit (--time=12:00:00
# above), so the job can still print a clear message instead of being
# killed silently by Slurm. 11h40m = 700 minutes. Change it together
# with --time.
RUN_TIMEOUT="700m"

# The graph to test.
GRAPHS=(
    "/scratch/d/dimopoul/graphs/com-Friendster/com-Friendster.mtx"
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
    # name for the human-readable progress message below -- it has
    # no effect on the CSV contents themselves, since the program
    # prints the full original path anyway.
    name=$(basename "$graph" .mtx)
    echo "=== $name ==="

    for t in $THREAD_COUNTS; do
        echo "  threads=$t"

        # OMP_NUM_THREADS is read by the OpenMP runtime at the
        # moment "$BIN" starts, not before -- so re-exporting it
        # here, once per inner-loop iteration, correctly changes the
        # thread count used by the NEXT invocation of the program.
        export OMP_NUM_THREADS=$t

        # Run the program exactly once. Its stdout includes exactly
        # one line starting with the literal text "CSV,", holding
        # every field we need, already comma-separated in the same
        # column order as the header written above. The pipeline
        # below:
        #   1. "grep '^CSV,'"     keeps ONLY the one line that starts
        #                         with "CSV," out of the ~10 lines
        #                         the program actually prints
        #   2. "sed 's/^CSV,//'"  strips that leading "CSV," prefix,
        #                         since the master file's header row
        #                         (written once, above) already
        #                         labels every column
        #   3. ">> $MASTER_CSV"   appends the resulting single line
        #                         as one new row of the master file
        #
        # The only difference from ifub_scaling_sweep.sh is the -v
        # flag: this run is very long, and -v makes the program print
        # its [progress] lines to stderr (which Slurm writes into the
        # slurm_<jobid>.out file), so you can tell a slow run from a
        # stuck one. It does not change stdout, so the CSV row is the same.
        #
        # The run is wrapped in `timeout` (see RUN_TIMEOUT above). In a
        # pipeline, $? would be the exit code of the LAST command (sed),
        # so PIPESTATUS[0] is used to read the exit code of the program
        # itself: 124 means it was stopped by the timeout, any other
        # non-zero value means the program failed. In both cases no CSV
        # row is written, so the message below is the only trace.
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