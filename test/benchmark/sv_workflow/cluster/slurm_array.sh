#!/usr/bin/env bash
# SLURM array job:  array element i runs tasks i, i+N, i+2N, ... of the
# design (N = number of array elements), all four arms per task.
# Finished task x arm results are skipped, so the job can be resubmitted
# after a time-out or node failure.
#
# USAGE (from test/benchmark/sv_workflow)
#   N=500
#   sbatch --array=0-$((N-1)) --export=ALL,STRIDE=$N,DESIGN=designs/default.json,\
#CONFIG=config.json,OUT=$SCRATCH/svbench-results cluster/slurm_array.sh
#
# Each task runs us_mpi_analysis with the "mpirun" command of the config,
# e.g. "srun --mpi=pmix -n 8" or "mpirun -np 8"; request the same number
# of tasks/cores here.  RUN_ARGS adds options of 'svbench run', e.g.
# RUN_ARGS=--retry-failed to run failed arms again.  No memory is requested (about 0.4 GB per MPI rank
# suffices); add e.g. --mem-per-cpu=1G on clusters that schedule memory.
#SBATCH --job-name=svbench
#SBATCH --ntasks=8
#SBATCH --cpus-per-task=1
#SBATCH --time=24:00:00
#SBATCH --output=svbench-%A_%a.log

set -euo pipefail
: "${STRIDE:?set STRIDE to the array size}"
: "${DESIGN:?}" "${CONFIG:?}" "${OUT:?}"
PYTHON=${PYTHON:-python3}

cd "${SLURM_SUBMIT_DIR:-.}"
"$PYTHON" -m svbench run "$DESIGN" "$CONFIG" "$OUT" \
    --index "$SLURM_ARRAY_TASK_ID" --stride "$STRIDE" \
    ${TIMEOUT:+--timeout "$TIMEOUT"} ${RUN_ARGS:-}
