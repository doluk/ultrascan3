#!/usr/bin/env bash
# Worker for a shared cluster:  claims the next free task of a task file
# (svbench split) and runs tasks for at most MAX_MINUTES (default 30;
# a task started before that runs to its end, about 30-60 min).  If work
# is left, the worker submits its successor and ends, so each job is
# short and other users' jobs get freed cores within minutes; the
# successor's --nice keeps its priority below theirs.
#
# USAGE (from test/benchmark/sv_workflow, on the shared cluster)
#   sbatch --array=1-32 \
#     --export=ALL,DESIGN=designs/cluster64.json,CONFIG=config.json,\
#OUT=$PWD/svbench-c64-node1,TASKS=node1.tasks cluster/slurm_worker.sh
#
#   Stop all chains gracefully:   touch $OUT/.stop
#   (running tasks finish; no new tasks, no successors)
#
# Each worker runs us_mpi_analysis with the "mpirun" of the config; keep
# --ntasks equal to its -np.  RUN_ARGS adds options of 'svbench run'.
#SBATCH --job-name=svbench-w
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --cpus-per-task=1
#SBATCH --time=02:00:00
#SBATCH --nice=100
#SBATCH --output=svbench-w-%j.log

set -uo pipefail
: "${DESIGN:?}" "${CONFIG:?}" "${OUT:?}" "${TASKS:?}"
PYTHON=${PYTHON:-python3}
MAX_MINUTES=${MAX_MINUTES:-30}
WORKER_SCRIPT=${WORKER_SCRIPT:-cluster/slurm_worker.sh}

cd "${SLURM_SUBMIT_DIR:-.}"

# A node that cannot run the benchmark (no Python/numpy, a program or
# library missing) would fail every task it takes:  hand the chain on to
# another node instead (at most 3 hops).
handoff() {
  host=$(hostname -s)
  hops=${SVB_BAD_HOPS:-0}
  echo "ERROR on $host: $1"
  if [ "$hops" -lt 3 ] && [ ! -e "$OUT/.stop" ]; then
    echo "resubmitting without $host"
    SVB_BAD_HOPS=$((hops + 1)) sbatch --export=ALL --exclude="$host" \
        "$WORKER_SCRIPT"
  fi
  exit 1
}
"$PYTHON" -c "import numpy" 2>/dev/null \
  || handoff "$PYTHON with numpy not available"
"$PYTHON" -m svbench check "$CONFIG" || handoff "node check failed"
export SVB_BAD_HOPS=0

"$PYTHON" -m svbench run "$DESIGN" "$CONFIG" "$OUT" --all \
    --task-file "$TASKS" --claim --max-minutes "$MAX_MINUTES" \
    ${TIMEOUT:+--timeout "$TIMEOUT"} ${RUN_ARGS:-}
rc=$?

[ "$rc" -eq 4 ] && handoff "simulations fail on this node"
if [ "$rc" -eq 3 ] && [ ! -e "$OUT/.stop" ]; then
  echo "work left: submitting successor"
  sbatch --export=ALL "$WORKER_SCRIPT"
fi
# 0 = no task left to claim, 1 = some arm failed (see the results)
exit $(( rc == 3 ? 0 : rc ))
