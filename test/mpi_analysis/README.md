# us_mpi_analysis regression tests

End-to-end tests that run `us_mpi_analysis` under `mpirun` on synthetic data
and compare the fitted models with stored baselines.

## Contents

| File | Purpose |
|---|---|
| `us_mpi_testdata.cpp` | Writes a simulated velocity dataset (`.auc` + edit file) and a custom-grid model. Three species, 30 scans, 60 krpm, fixed-seed noise (σ = 0.004). |
| `us_mpi_regression.py` | Builds the job archive (hpcrequest XML in the LIMS format), runs the case, reduces `analysis-results.tar` to a JSON summary and compares it with `baseline/<case>.json`. |
| `baseline/*.json` | Reference summaries: per model the RMSD, the signal-weighted moments of s, f/f0 and vbar, the solute list, and any TI/RI noise vectors. |

## Cases

| Case | Path exercised | Ranks | Comparison |
|---|---|---|---|
| `2dsa_basic` | `_2dsa_master`, uniform grid, 9 subgrids | 2 | exact |
| `2dsa_refine` | iterative refinement (3) | 2 | exact |
| `2dsa_mc` | Monte Carlo (3) | 2 | exact |
| `2dsa_meniscus` | meniscus fit (3 points) | 2 | exact |
| `2dsa_noise` | TI + RI noise | 2 | exact |
| `2dsa_customgrid` | custom grid (s × vbar) | 2 | exact |
| `2dsa_global` | global fit, 2 datasets | 2 | exact |
| `2dsa_composite` | composite job, 2 datasets | 2 | exact |
| `2dsa_refine_np4`, `2dsa_mc_np4` | several workers | 4 | loose |
| `2dsa_mc_pmasters` | `pm_2dsa_master` (MC, 2 groups) | 6 | loose |
| `2dsa_composite_pmasters` | `pm_2dsa_cjmast` (2 groups) | 6 | loose |
| `pcsa_sl` | PCSA straight lines | 2 | exact |
| `ga_basic` | GA, 3 buckets | 4 | loose |

"Exact" means RMSD within 1e-6 relative, moments within 1e-4 relative and
noise within 1e-6 absolute.  These cases use a single worker, because with
several workers the 2DSA master merges subgrid results in arrival order and
GA migrates genes asynchronously, so results then depend on message timing.
"Loose" cases only require the same output files and an RMSD not more than
15 % above the baseline.

## Running

The tests need a build with MPI, programs and tests, without the database:

```bash
cmake -S . -B build -G Ninja -DUS3_NO_DB=ON -DUS3_BUILD_PROGRAMS=ON \
      -DBUILD_TESTING=ON -DUS3_PREFER_STATIC=ON -DBUILD_DOCUMENTATION=OFF
cmake --build build
ctest --test-dir build -L mpi --output-on-failure
```

Single cases can be run directly:

```bash
python3 test/mpi_analysis/us_mpi_regression.py check 2dsa_mc \
    --program build/bin/us_mpi_analysis --testdata build/bin/us_mpi_testdata
```

## Updating baselines

Only when a change of results is intended:

```bash
python3 test/mpi_analysis/us_mpi_regression.py update all \
    --program build/bin/us_mpi_analysis --testdata build/bin/us_mpi_testdata
```

Review the JSON diff before committing.
