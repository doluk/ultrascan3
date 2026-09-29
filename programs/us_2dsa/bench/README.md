# 2DSA subgrid benchmark

Benchmark of the subgrid partitions discussed in the internal paper
*Subgrid coverage in the parallel two-dimensional spectrum analysis*
(2dsa_subgrid_coverage_v1): the current custom-grid rule (component `k` in
subgrid `k mod n`), the classic shifted grid, general sublattices chosen in
the finite-element (FEM) distance metric (Stage 2), and grids placed in that
metric (Stage 3). Everything runs through UltraScan code:

* **Data**: `us_astfem_sim` (its `US_Astfem_Sim` class, driven through
  `init_from_args()` exactly as its command line does) simulates each mixture
  and saves the AUC data, edit, run definition, TimeState and TI/RI noise.
* **Fits**: the `us_2dsa` processing classes (`US_2dsaProcess`, worker
  threads, ASTFEM, NNLS with TI+RI noise) with a CUSTOMGRID model. The
  partition is encoded in the component order (member `m` of subgrid `i` at
  position `m*n + i`), which the unchanged fit code decodes with `k mod n`.
  Depth-1+ merging uses the `2DSA-OrderedMerge` debug setting (task order
  rather than thread-arrival order) so that runs are reproducible.
* **FEM metric** (Stages 2 and 3): ASTFEM simulations of a 127 x 127
  candidate set on the same experiment, TI and RI components projected out.

## Build

```
cmake -S . -B build -DUS3_BUILD_2DSA_BENCH=ON ...
cmake --build build --target us_2dsa_bench
```

The benchmark links the `us_2dsa` process sources and the `us_astfem_sim`
class (its `main()` is renamed at compile time). It runs headless
(`QT_QPA_PLATFORM=offscreen`).

## Experiment and data

* 50,000 rpm, 400 rpm/s acceleration, 20 C, water, vbar 0.73 mL/g; meniscus
  5.9 cm, bottom 7.2 cm, no rotor stretch; 60 scans every 300 s from 300 s to
  18,000 s; edits 5.951-7.1 cm at 0.003 cm (384 points).
* Noise (us_astfem_sim percentages of the total concentration, 1.0 OD):
  random 0.3 % (sigma 0.003), time-invariant 0.05 % per point (random walk),
  radially invariant 0.5 %. Fits include TI and RI noise.
* Mixtures (s [S], f/f0, concentration):
  * M1: (3.3, 1.25, 0.4), (6.1, 1.55, 0.6)
  * M2: (2.2, 1.4, 0.3), (4.7, 1.3, 0.4), (8.4, 2.6, 0.3)
  * M3: (2.5, 1.2, 0.5), (5.5, 3.5, 0.5)
  * H1: (4.3, 1.3, 0.95), (6.4, 1.45, 0.05)
  * H2: 12 solutes on a line from (2.0, 1.2) to (8.0, 2.8), 1/12 each
  * H3: (4.0, 1.2, 0.5), (4.0, 2.5, 0.5)
  * A1: (6.5, 1.5, 0.90), (9.5, 1.6, 0.07), (3.5, 1.4, 0.03)
* Three noise realizations per mixture, plus one noise-free run used as the
  truth for the fitted-signal error.

## Configurations (64 subgrids each)

| name | fine grid | partition |
|---|---|---|
| modulo | 64 x 64 uniform, s 1-10 S, f/f0 1-4, row-major order | current rule, `k mod 64` (one s value per subgrid) |
| classic8x8 | 64 x 64 | classic shifted grid, 8 x 8 offsets (Stage 1 choice in index units) |
| ugrid8 | 64 x 64 | regular-grid path of us_2dsa (`init_solutes`, 8 repetitions) |
| rect4x16 | 64 x 64 | best rectangle in the FEM metric (periods 4 in s, 16 in f/f0) |
| lattice | 64 x 64 | best sublattice in the FEM metric, Hermite normal form (8, 3, 8) (Stage 2) |
| g128x32_8x8 | 128 x 32 uniform | 8 x 8 offsets |
| g128x32_lattice | 128 x 32 uniform | best sublattice in the FEM metric, (16, 6, 4) |
| fem4096 | 4096 points placed by farthest-point sampling in the FEM metric | interleaved farthest-point partition (Stage 3) |
| fem1024 | 1024 points, as above | interleaved farthest-point partition (Stage 3, reduced) |

Further partition cases (`gen_cases.py`, `gen_ref.py`, `bench_run.py gen`):
a 100 x 100 grid with 101 subgrids, 64 x 64 with 41, 60 x 60 with 60, and a union
of two partial grids with 32 subgrids, each with the current modulo rule, the
best sublattice in index units (Stage 1, Algorithm 1 of the proposal including
size balancing) and in the FEM metric (Stage 2); the union also with the
interleaved farthest-point partition. These are the cases in which the general
sublattice rule differs from the classic shifted grid.

Single-pass fits use one refinement iteration; iterated fits use up to 10
(stopping when the solutes no longer change). The full-grid optimum (exact
NNLS over all 4096 points of the 64 x 64 grid, TI+RI projected out) is
computed from the same ASTFEM columns in Python as the accuracy reference.

## Measures

* `rmsd`: residual RMSD of the fit; `excess`: relative excess over the
  full-grid optimum of the same dataset.
* `signal_err_rmsd`: RMSD between the fitted signal and the noise-free data
  (TI and RI components projected out).
* `ws`, `wk`: Wasserstein distance between fitted and true distributions of
  s and f/f0; `sp_dc`: largest concentration error of any true species.
* `wall_s`, `cpu_user_s`: fit time (4 threads); `rss_peak_kb`: peak resident
  memory; `simulations`: ASTFEM solute simulations (task inputs summed);
  `ntasks`, `maxdepth`, `merge_in_max`: merge tree; `surv0`: solutes
  surviving the subgrid fits.

## Reproduce

`run_all.sh` runs the whole pipeline (about 2.5 hours on 4 cores). The fits
with the corrected noise solve (debug option `SolveSim-ExactNoise`) are the same
matrix with `BENCH_DEBUG=SolveSim-ExactNoise BENCH_SET_SUFFIX=_exact`.

## Report

`report/subgrid_benchmark.typ` is the write-up of the results (Typst 0.15;
`python3 report/build.py` with the `typst` Python package). Its figures are
made by `report/make_figures.py` from the result files (`BENCH_DIR` pointing at
the benchmark directory).
