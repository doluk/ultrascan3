#!/bin/sh
# Reproduce the 2DSA subgrid benchmark (see README.md).
#
# Environment:
#   US_2DSA_BENCH  path of the us_2dsa_bench binary (default ../../../build/bin)
#   BENCH_DIR      working directory for inputs, grids, columns, results (default .)
#   BENCH_RESULTS  UltraScan results directory holding the simulated runs
#                  (default ~/ultrascan/results)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${US_2DSA_BENCH:-$HERE/../../../build/bin/us_2dsa_bench}
export LD_LIBRARY_PATH=$(dirname "$(dirname "$BIN")")/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export BENCH_DIR=${BENCH_DIR:-$(pwd)}
RES=${BENCH_RESULTS:-$HOME/ultrascan/results}
cd "$BENCH_DIR"
mkdir -p inputs grids cols out "$RES"

# 1. Models, buffer and simulation parameters; random noise 0.3 % of the total
#    concentration (1.0 OD) gives an RMSD near 0.003; TI and RI noise are fitted.
"$BIN" prepare inputs 0.3 0.05 0.5

# 2. us_astfem_sim datasets: 3 noise seeds and one noise-free run per mixture.
#    Edits start at 5.951 cm (60 scans x 384 points, 5.951-7.1 cm).
for m in M1 M2 M3 H1 H2 H3 A1; do
   "$BIN" simulate inputs/mix_$m.xml inputs/water.xml inputs/sp_clean.xml \
          "$RES/bench${m}clean" 1 5.951
   for s in 1 2 3; do
      "$BIN" simulate inputs/mix_$m.xml inputs/water.xml inputs/sp_noisy.xml \
             "$RES/bench${m}s$s" $((1000 * s + 7)) 5.951
   done
done

# 3. ASTFEM solute simulations for the FEM metric: 127 x 127 candidate set and
#    a uniform 128 x 32 grid (f/f0 outer loop, s inner loop)
python3 - <<'EOF'
import numpy as np
with open('proxy127.csv', 'w') as f:
    for k in np.linspace(1, 4, 127):
        for s in np.linspace(1, 10, 127):
            f.write(f"{s:.10g},{k:.10g}\n")
with open('grid128x32.csv', 'w') as f:
    for k in np.linspace(1, 4, 32):
        for s in np.linspace(1, 10, 128):
            f.write(f"{s:.10g},{k:.10g}\n")
EOF
"$BIN" columns "$RES/benchM1clean" proxy127.csv cols/proxy127.f32 4
"$BIN" columns "$RES/benchM1clean" grid128x32.csv cols/g128x32.f32 4

# 4. Partitions: current modulo rule, classic shifted grid, best rectangle,
#    single-subgrid (full) grid; then the FEM-metric grids (Stages 2 and 3)
python3 "$HERE/make_grids.py"
python3 "$HERE/fem_grids.py"

# 5. Fits (one at a time, 4 threads), full-grid reference, summary
python3 "$HERE/bench_run.py" single
python3 "$HERE/bench_run.py" arrival
python3 "$HERE/bench_run.py" iterated
python3 "$HERE/fullgrid_ref.py"
python3 "$HERE/analyze.py" single   > summary_single.md
python3 "$HERE/analyze.py" iterated > summary_iterated.md
python3 "$HERE/analyze.py" arrival  > summary_arrival.md

# 6. The same matrix with the corrected noise solve (debug option
#    SolveSim-ExactNoise), results in out/single_exact and out/iterated_exact
BENCH_DEBUG=SolveSim-ExactNoise BENCH_SET_SUFFIX=_exact python3 "$HERE/bench_run.py" single
BENCH_DEBUG=SolveSim-ExactNoise BENCH_SET_SUFFIX=_exact python3 "$HERE/bench_run.py" iterated

# 7. Partition cases where the general sublattice rule differs from the classic
#    grid (100x100/101, 64x64/41, 60x60/60, union of partial grids with 32):
#    columns, partitions, exact references, single-pass fits with both solvers
python3 "$HERE/gen_cases.py" points
"$BIN" columns "$RES/benchM1clean" cols/g100.csv cols/g100.f32 4
"$BIN" columns "$RES/benchM1clean" cols/g60.csv cols/g60.f32 4
python3 "$HERE/gen_cases.py" grids
python3 "$HERE/gen_ref.py"
python3 "$HERE/bench_run.py" gen
BENCH_DEBUG=SolveSim-ExactNoise BENCH_SET_SUFFIX=_exact python3 "$HERE/bench_run.py" gen
