# Global-fit study: known-truth tests for us_mpi_analysis global fits

This directory quantifies the defects found in the review of the
multi-dataset sedimentation velocity (SV) global fit, and compares the current
code with the proposed fixes. All data are synthetic: the true species,
concentrations, noise and detector behavior are known.

## Contents

| File | Purpose |
|---|---|
| `gf_testdata.cpp` | Data generator derived from `test/mpi_analysis/us_mpi_testdata.cpp`. Added options: `rpm=`, `sigma=`, `stray=` (stray-light detector nonlinearity, OD_obs = −log10((10^−OD + f)/(1 + f))), `odlim=` (edit ODlimit), `agg=` (a 150 S aggregate). |
| `gf_fixes.patch` | Fixes F1–F3 applied to `programs/us_mpi_analysis/us_mpi_analysis.cpp`; used only to build the comparison binary. |
| `run_e2e.py` | Runs the **real** `us_mpi_analysis` (current build and patched build) on the scenarios S0–S3 over several noise seeds, then scores every output model against the truth. |
| `summarize_e2e.py` | Mean ± SD table of the E2E results. |
| `gf_basis.cpp` | Exports ASTFEM basis matrices (A) of an s × f/f0 grid that contains the true species, through `US_SolveSim::calc_residuals`. Also runs the real global NNLS on given data. |
| `nnls_study.py` | Monte Carlo study (100 replicates per scenario) of method variants on the exported basis. |
| `validate_replica.py` | Checks that the Python replica in `nnls_study.py` reproduces the real `US_SolveSim` global fit. |
| `ga_fitness_study.py` | GA multi-dataset fitness formulas vs. true candidate quality. |
| `results/` | Raw JSON results and `e2e_table.md` (the full E2E table). |

## Truth and metrics

- **Species (20,W):** s = 2.5, 4.5, 7.0 S; f/f0 = 1.25, 1.6, 2.1; signal fractions 0.263, 0.421, 0.316.
- **Total concentration:** 0.95·scale OD.
- **Optional aggregate:** 150 S.
- **Run conditions:** 30 scans, 6.0–7.1 cm, 60 krpm unless stated otherwise. Noise σ = 0.004 OD unless stated otherwise.
- **Metrics:**
  - relative error of the model total concentration;
  - signal fraction per species, integrated over s windows [0, 3.5), [3.5, 5.75), [5.75, 50) S;
  - signal-weighted mean s;
  - reported RMSD vs. the true σ.
- **Model kinds written by `write_global`:**
  - SG: superglobal model;
  - SC.k: global solutes scaled to dataset k;
  - VR.k: model refitted to dataset k.

Method variants in `nnls_study.py`:

| Variant | Definition |
|---|---|
| `cur` | Current code: data divided by the given c_e; ODlimit compared with the normalized data; no weights. This is an implicit weight of 1/c_e on the original-unit residuals. |
| `F1` | ODlimit scaled with the data, so points are excluded at OD ≥ ODlimit. |
| `W` | F1, with residuals weighted by 1/σ_e (true σ). |
| `What` | F1, with σ_e estimated from a single-dataset fit (two-stage). |
| `Wa` | W, with a fitted concentration factor α_e per dataset (α_0 = 1; alternating least squares). |

Replica validation (`validate_replica.py`): the Python NNLS reproduces the
real `calc_residuals` concentrations to within 7·10⁻¹² in all four cases tested:
current and F1 masking, with ODlimit 1.5 and with ODlimit 0.8 (2393 + 2400
points masked). The column norm cut of `US_SolveSim` (‖A_col‖ < 1) had to be
replicated to reach this agreement.

## Results — real us_mpi_analysis (E2E, mean over seeds)

`cur` = current build, `fix` = build with `gf_fixes.patch`. Full table:
`results/e2e_table.md`.

| Scenario | Model | Quantity | cur | fix |
|---|---|---|---|---|
| S0 baseline (c = 0.95, 0.285; σ = 0.004) | SC.1 | reported RMSD | 0.01402 | 0.00399 |
| | SG | reported RMSD | 0.01035 | 0.00400 |
| | all | parameters | identical | identical |
| S1 c = 2.09, 0.57; 0.5 % stray light; ODlimit 1.5 | SG/SC | total error | −4.61 % | −0.94 % |
| | SG/SC | s̄ error | −1.81 % | +0.34 % |
| | SG/SC | max \|Δ fraction\| | 0.020 | 0.005 |
| S1b same, linear detector | SG/SC | all metrics | ≈ 0 | ≈ 0 |
| | VR.0 | total / s̄ error | 0.0 % / 0.0 % | **+1.2 % / +0.67 %** |
| S1c ODlimit 0.8, c = 0.95, 0.38, stray light | SG/SC | total / s̄ error | −1.73 % / −0.53 % | −0.77 % / +0.14 % |
| | VR.1 | total error | −1.90 % | −0.76 % |
| S3 c_B given 10 % too high | SC.0 / SC.1 | total error | −4.6 % / +5.0 % | same |
| S2 GA, 60 krpm (aggregate pelleted) + 20 krpm | VR.1 | solutes found (of 4) | **2** | 4 |
| | VR.1 | total error | −8.3 % | −0.01 % |
| | VR.1 | max \|Δ fraction\| | 0.29 | 0.005 |
| | VR.1 | aggregate signal | 0 | 0.095 (truth 0.095) |

Notes:

- **S1, current code:** no point is excluded. Dataset A has 46 % of its points above ODlimit 1.5, but the normalized data never exceed 1.5.
- **S1c, current code:** the plateau of the dilute dataset is excluded as well.
- **S1b:** with an ideal detector, F1 discards valid data. The global models are unaffected, but the single-dataset refit VR.0 gets slightly worse. Here the current code behaves better.
- **S2, VR.0:** the −10 % total in VR.0 is expected and the same in both builds; the aggregate cannot be observed at 60 krpm.
- **S2, SC models:** SC.1 is not affected in this case only because both datasets have the same concentration.
- **S1 and S1c, fixed build, reported RMSD:** the SC.0 RMSD is 0.0887 against a true σ of 0.004. This is a new finding, F7: `US_SolveSim` computes residuals and variances over *all* points, including the points excluded by the OD limit (`utils/us_solve_sim.cpp`, residual loop). Before F1 this was hidden because no points were excluded.

## Results — NNLS study (100 replicates; RMSE of the 3 fractions, bias of s̄ and totals)

| Scenario | cur | F1 | W (=What) | Wa |
|---|---|---|---|---|
| X1a ODlimit, stray light | 0.0140, s̄ −1.80 %, tot −4.6 % | 0.0047, +0.36 %, −0.9 % | 0.0085, +0.98 %, −0.9 % | **0.0018**, −0.14 %, −2.9/−1.0 % |
| X1b ODlimit, linear | 0.0012 | 0.0013 | 0.0006 | 0.0006 |
| X1c ODlimit 0.8, stray light | 0.0048, −0.57 %, −1.8 % | 0.0018, +0.15 %, −0.8 % | 0.0016, +0.40 % | 0.0019, −0.12 % |
| X2a equal σ, c = 0.95 / 0.10 | 0.0067 | 0.0067 | **0.0012** | 0.0012 |
| X2b σ ∝ c | 0.0010 | 0.0010 | 0.0010 | 0.0010 |
| X2c σ = 0.004 / 0.012 | 0.0019 | 0.0019 | 0.0013 | 0.0013 |
| X2d 60 k + 30 krpm | 0.0016 | 0.0016 | 0.0014 | 0.0013 |
| X3a c_B +10 %, same speed | 0.0015, tot −4.5/+5.0 % | same | 0.0012, tot −2.1/**+7.7 %** | 0.0012, tot 0/0 |
| X3b c_B −10 %, 60 k + 30 krpm | **0.0388, s̄ +8.3 %**, tot +9.0/−1.9 % | same | 0.0224, +5.1 % | **0.0010, +0.01 %, 0/0** |

GA fitness (`ga_fitness_study.py`; 20 populations × 60 candidates per scenario):

- **Ranking:** the formulas var/maxod (current), var/maxod², the NNLS objective and the likelihood rank candidates almost identically. Their pairwise concordance with the true quality differs by ≤ 0.005 (0.66–0.70), and the selected best candidate is the same.
- **Why:** with normalized data, maxod_e ≈ 1 for every dataset. Even after F1 changes one maxod to 0.72, the ranking does not change, because misfit from wrong solutes dominates every dataset's variance.

## Conclusions

1. **F1 (OD limit not scaled): confirmed and relevant.**
   - With a nonlinear detector, the current code includes saturated data. The global composition is biased (s̄ −1.8 %, totals −4.6 %); with F1 it is +0.36 % and −0.9 %.
   - With a low ODlimit, the current code over-excludes data.
   - With an ideal detector, F1 only discards information: the global models are unchanged and the single-dataset refit of the concentrated set is slightly worse. Excluding these points is still correct, because the ODlimit exists for real detector nonlinearity.
2. **F2 (`nsolutes` not reset in GA/PCSA `write_global`): confirmed.**
   - When a column is cut for an earlier dataset, the refit model of the later dataset silently loses solutes. In S2 it lost the aggregate and the 2.5 S species: total −8.3 %, a fraction error of 0.29.
   - It only occurs when a solute is invisible in an earlier dataset, for example multi-speed data with aggregates.
3. **F3 (variance units): confirmed and deterministic.**
   - The reported RMSD of dataset k is σ/c_k: 3.5× too high for c = 0.285.
   - The fitted parameters are unaffected.
4. **F7 (new):** after F1, the reported RMSD includes the excluded points (0.089 vs. 0.004). This needs a matching change in `US_SolveSim` before F1 is shipped.
5. **σ-weighting:**
   - It helps markedly when datasets differ in concentration but not in σ: 5.6× lower fraction RMSE in X2a. It also helps when datasets come from different optical systems.
   - It gives no gain when σ ∝ c; there the current implicit weighting is already optimal.
   - **It is worse than F1 alone under model error:** in X1a the residual stray-light bias of the concentrated set gets more weight. In X3a it shifts the total-concentration error onto the dilute set.
   - Two-stage σ estimation performs like the oracle.
6. **α_e fit (concentration factors):** this is the largest improvement in the presence of concentration errors.
   - In multi-speed global fits, a 10 % error in a supplied total concentration biases s̄ by +8.3 % in the current code; the α-fit reduces this to 0.01 %.
   - α also absorbs part of the detector nonlinearity (X1a: lowest composition error).
   - In X1a, ds0's total is biased (−2.9 %) because α_0 is fixed at 1.
   - Weighting alone, without α, should not be shipped.
7. **GA fitness formula:** the dimensional inconsistency has no measurable effect on selection. It is low priority.

## Reproducing

Build a headless (HPC profile, Qt5) tree, then compile the tools against it:

```bash
cmake -S . -B ../build-gf -G Ninja -DUS3_PROFILE=HPC -DCMAKE_BUILD_TYPE=Release
ninja -C ../build-gf us_mpi_analysis
# Same for a worktree with gf_fixes.patch applied -> ../build-fix
FL="-O2 -std=c++17 -fPIC -DLINUX -DNO_DB -DUS_UTIL_STATIC -Iutils -I../build-gf -I../build-gf/utils $(pkg-config --cflags Qt5Core Qt5Xml Qt5Network)"
LIBS="-Wl,--whole-archive ../build-gf/lib/libus3_resources.a -Wl,--no-whole-archive ../build-gf/lib/libus_utils4_static.a $(pkg-config --libs Qt5Xml Qt5Network Qt5Core Qt5Sql) -lssl -larchive -lcrypto"
g++ $FL test/global_fit_study/gf_testdata.cpp -o ../build-gf/bin/gf_testdata $LIBS
g++ $FL test/global_fit_study/gf_basis.cpp    -o ../build-gf/bin/gf_basis    $LIBS

# E2E (needs mpirun)
python3 test/global_fit_study/run_e2e.py --testdata ../build-gf/bin/gf_testdata \
   --program cur=../build-gf/bin/us_mpi_analysis --program fix=../build-fix/bin/us_mpi_analysis \
   --reps 6 --out e2e.json
python3 test/global_fit_study/summarize_e2e.py e2e.json

# NNLS study (numpy, scipy)
mkdir basis && cd basis
for r in 60000 30000; do gf_testdata data . r$r 1 1 1.0 rpm=$r; gf_basis basis . r$r basis_$r; done
gf_testdata data . r60000b 1 2 1.0 rpm=60000
python3 ../test/global_fit_study/validate_replica.py . ../../build-gf/bin/gf_basis
python3 ../test/global_fit_study/nnls_study.py . --reps 100
python3 ../test/global_fit_study/ga_fitness_study.py .
```

Limitations:

- Ideal non-interacting species on the grid.
- No TI/RI noise.
- A single-pass 2DSA grid in the NNLS study; the E2E runs use the real 2DSA with 3 uniform subgrids.
- 6 (2DSA) or 4 (GA) seeds per E2E scenario. The GA is stochastic, but the F2 effect is identical across seeds.
