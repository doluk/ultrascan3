# SV workflow benchmark: NNLS (main vs branch) × workflow (old vs new)

The question this benchmark answers: now that the branch removes TI and RI
noise exactly in its NNLS step, is the first TI-only 2DSA fit of the
standard sedimentation velocity (SV) workflow still needed?

| Arm | `us_mpi_analysis` build | Workflow |
|---|---|---|
| `main_old`   | main `5a137ef` (old NNLS) | old |
| `main_new`   | main `5a137ef` (old NNLS) | new |
| `branch_old` | this branch (new NNLS)    | old |
| `branch_new` | this branch (new NNLS)    | new |

**Old workflow**
1. 2DSA with TI noise. This gives the TI noise N1.
2. 2DSA meniscus+bottom fit with TI and RI noise, with N1 loaded.
3. Fit-meniscus: write the fitted meniscus and bottom into the edit.
4. 2DSA iterative refinement with TI and RI noise, with N1 loaded.

**New workflow:** steps 2 to 4, without step 1 and without any loaded noise.

Expected result: with exact TI/RI elimination, a pre-subtracted TI vector lies
in the subspace that is projected out. The branch's old and new workflows should
therefore give the same result up to numerical noise. The benchmark tests
this, and also measures how much the old NNLS depends on step 1.

Every task (one condition, one replicate) is simulated once. All four arms
analyse the same data (a paired design), and the arms are compared on the same
task.

## Pipeline of one task

1. **Simulation** (`us_astfem_sim`, CLI). The task is simulated twice with the
   same scan schedule: once with the task's noise and seed, once without any
   noise. The noisy data, the time state and the edit go to the analyses. The
   noise-free data and the true TI/RI noise CSV files are the ground truth.
2. **Edit.** The simulator's edit covers the full column and has the true
   meniscus and bottom. It is rewritten as follows:
   - The meniscus and bottom get a random error of up to ±`edit.meniscus_offset_max` and ±`edit.bottom_offset_max` (0.01 cm). The error is the same for all noise levels of a given system, speed and replicate.
   - The data range starts at edit meniscus + 0.025 cm. This leaves room for the meniscus grid (±0.015 cm).
   - The data range ends `range_end` cm before the edit bottom. The smallest value, 0.03 cm, includes the back-diffusion region.
3. **Analyses** (`us_mpi_analysis`). Jobs are submitted exactly as the LIMS would submit them: a tar file with the hpcrequest XML, AUC, edit, time state and noise files.
   - 2DSA settings are the UltraScan defaults: 64×64 grid, 8 grid repetitions, f/f0 from 1 to 4, meniscus/bottom range 0.03 cm with 11 points (11×11 = 121 fits), refinement with at most 3 iterations, 200 simulation points.
   - The s range is set per system to [0.7·s_min, 1.3·s_max], because the default 1–10 S cannot cover all systems.
4. **Fit-meniscus.** `svbench/fitmen.py` re-implements `US_FitMeniscus::plot_3d` and `edit_update`. It takes the grid point with the lowest RMSD and its (up to 8) neighbours, averages them with weights 1/rmsd − 1/rmsd_max, rounds to 5 decimals, and replaces the edit's meniscus and bottom. Like the GUI, it refuses an update when the meniscus would reach into the data range.
5. **Evaluation** (`svbench/evaluate.py`). The metrics are listed below.

## Factors

| Factor | Levels |
|---|---|
| system | 13 systems (below) |
| speed (rpm) | 5000, 11000, 17000, 23000, 29000, 36000, 42000, 48000, 54000, 60000 |
| `ti_noise` (% of total conc., per radial step of a random walk) | 0, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0 |
| `ri_noise` (% of total conc., per scan) | 0, 0.5, 1, 2, 5, 10, 25 |
| `random_noise` (% of total conc.) | 0, 0.25, 0.5, 1, 2, 5 |
| `local_noise` (% of local conc.) | 0, 0.5, 1, 2, 5, 10 |
| `baseline` (OD) | 0, 0.01, 0.05, 0.2, 0.5, 1.0 |
| `range_end` (cm before bottom) | 0.03, 0.06, 0.12, 0.25 |

The noise levels are the parameters of `us_astfem_sim`. Its TI noise is a
cumulative random walk, so 1 % per step amounts to tens of percent across the
column. That is the extreme end.

Systems (`svbench/systems.py`; s in S / f/f0 / fraction):

| id | solutes |
|---|---|
| S01_single | 4.4/1.25 |
| S02_far_apart | 2/1.3, 20/1.5 |
| S03_same_s_diff_ff0 | 5/1.2, 5/2.4 |
| S04_same_ff0_diff_s | 4/1.4, 6/1.4 |
| S05_very_small_s | 0.3/1.2 |
| S06_very_large_s | 150/1.1 |
| S07_three_ladder | 2.5/1.2, 4.5/1.3, 7/1.4 |
| S08_four_oligomers | 4.3/1.25, 6.4/1.3, 8.4/1.35, 10.2/1.4 (0.4/0.3/0.2/0.1) |
| S09_five_wide | 1/1.2, 3/1.4, 8/1.6, 20/1.8, 40/2.0 |
| S10_six_mixed_shapes | 2/1.2, 2/2.5, 6/1.3, 6/3.0, 15/1.5, 40/1.8 |
| S11_three_large | 30/1.3, 80/1.2, 200/1.1 |
| S12_four_small | 0.5/1.2, 1/1.3, 1.8/1.4, 3/1.5 |
| S13_trace_aggregate | 4.4/1.25 (85 %), 6.6/1.35, 9/1.45, 12/1.6 |

**Scan schedule** (`physics.scan_schedule`). There are 50 scans. The last scan is taken when the slowest solute's boundary has crossed 80 % of the data range, and the first when the fastest solute's has crossed 10 %. Times are limited to between the end of acceleration + 30 s and 30 h, and scans are at least 20 s apart. Slow solutes at low speed therefore give diffusion-dominated data, and fast solutes at high speed can pellet before the first scan, as they would in a real run.

### Designs

| File | Content | Conditions | Tasks (×5 replicates) |
|---|---|---|---|
| `designs/default.json` | One-factor-at-a-time sweeps of every noise and range factor around the reference, for every system × speed, plus a crossed TI × RI × random-noise block (3 speeds) | 5083 | 25 415 |
| `designs/full_factorial.json` | Every combination of all levels | 5 503 680 | 27 518 400 |
| `designs/smoke.json` | Pilot: 2 systems × 2 speeds × 2 TI levels, 5×5 meniscus/bottom grid, 2 replicates | 8 | 16 |

Reference condition: TI 0.05, RI 1, random 0.5, local 0, baseline 0.05,
range_end 0.12. `python -m svbench count DESIGN` prints the size of a design.
Designs can inherit from each other (`"inherit"`), so a custom design only
needs to list what it changes.

## Metrics (per task and arm)

- **Analysis RMSD** and `rmsd_ratio` = RMSD / RMS of the true random noise. The true random noise is noisy − noise-free − TI − RI − baseline. A value of 1 is ideal; below 1 means over-fitting.
- **Noise recovery.** TI and RI are only determined up to a common constant, and the baseline is a constant RI noise. The fitted C_fit = TI_fit(r) + RI_fit(t) is therefore compared with C_true = TI_true(r) + RI_true(t) + baseline:
  - `noise_rmsd`: the overall error.
  - `ti_shape_rmsd` and `ri_shape_rmsd`: the error of each vector after centring.
  - `noise_offset`: the constant part of the error.
  - `corrected_data_rmsd`: the data with the fitted noise removed, compared with the noise-free signal.
- **Concentration and accuracy.**
  - Total fitted concentration against the truth (`total_conc_relerr`).
  - Each fitted solute is assigned to the nearest true solute in (ln s, f/f0). For each true solute the benchmark reports the concentration error and the errors of the concentration-weighted s, f/f0, D and MW (max and mean over solutes).
  - `ghost_conc_fraction`: the share of concentration lying more than 3 units (10 % in s, 0.2 in f/f0) from any true solute.
- **Meniscus and bottom** errors after the fit, whether the minimum lies on the edge of the grid, and whether the edit update was accepted.
- **Wall time** of each step.

`aggregate` writes `runs.csv` (task × arm), `species.csv`, `conditions.csv`
(mean and SD over the 5 replicates per condition × arm), `paired.csv` (mean
and SD of each arm's paired difference to `main_old`) and `errors.csv`.
`report` turns these into a self-contained HTML report with headline
medians, win rates against `main_old`, paired old/new and main/branch
differences, one-factor sweeps, and tables by system and by speed.

## Running

Requirements: Python ≥ 3.6 with numpy (matplotlib is needed only for
`report`), MPI, qmake with Qt5 (and the libraries of a normal UltraScan build), and an UltraScan settings file with a valid registration.
`us_astfem_sim` checks the license.

```bash
# 1. build main/branch us_mpi_analysis and us_astfem_sim with qmake (Qt5);
#    pass the cluster's local.pri of the us_mpi_analysis (NO_DB, MPI) build
#    and of a GUI build.  Prints the wrapper paths for config.json.
QMAKE=/opt/qt-5.15.10-qwt-6.1.6/bin/qmake \
  cluster/build.sh $HOME/svbench-build local-mpi.pri local.pri
# 2. config:  copy config.example.json, fill in the paths
# 3. inspect the design
python3 -m svbench count designs/default.json
python3 -m svbench list  designs/default.json --out tasks.tsv
# 4a. one task locally (all four arms)
python3 -m svbench run designs/default.json config.json results --index 0
# 4b. the full design on SLURM:  N array elements, each runs every N-th task
N=1000
sbatch --array=0-$((N-1)) \
  --export=ALL,STRIDE=$N,DESIGN=designs/default.json,CONFIG=config.json,OUT=$SCRATCH/svbench \
  cluster/slurm_array.sh
# 5. collect and report (any time; finished runs are skipped on resubmit)
python3 -m svbench aggregate $SCRATCH/svbench --out summary
python3 -m svbench report summary --out summary/report.html
```

`build.sh` makes three git worktrees (main and branch for
`us_mpi_analysis`, branch for `us_astfem_sim`), each with its own `lib/`.
The executables are therefore called through wrapper scripts in
`DEST/bin` that set `LD_LIBRARY_PATH`. The wrappers also work under
`mpirun`/`srun`.

Run the commands from `test/benchmark/sv_workflow`, or put that directory
on `PYTHONPATH`. Each task uses its own scratch directory, which holds a
private UltraScan settings store and work tree. The scratch directory is
deleted afterwards unless `--keep` is given. Results are written as
`OUT/<task>/<arm>.json`, and existing results are not recomputed.

### Cost

Measured in the container: one 2DSA fit with 50 scans × ~1150 points takes
≈19 s on 4 MPI ranks. One arm with a 5×5 meniscus/bottom grid takes
≈4.4 min. With the default 11×11 grid an arm is dominated by the 121
meniscus/bottom fits, which comes to roughly 15–20 min on 4 ranks. A task
(4 arms) then needs about 1.2 h on 4 cores, or about 5 core-hours, and the
default design needs on the order of 10⁵ core-hours. The cost varies with
speed and system, because the number of scans is fixed while the run length
is not. Options for reducing it:
- `analysis.meniscus_points` (e.g. 7 instead of 11).
- `replicates`.
- Fewer speeds or systems, via an inheriting design file.

## Changes to `us_astfem_sim` made for this benchmark

- `--seed N` seeds the noise generator. Without it, `QRandomGenerator` starts from its fixed default seed, so every run had identical noise.
- `--odlimit X` sets the OD limit applied when saving. The default is 2 × total concentration; 0 means no limit. The benchmark uses 5 × total concentration. A higher limit coarsens the 16-bit AUC quantisation, (max − min)/65535; the default clips the back-diffusion region near the bottom.
- With `--save` from the command line, the "OD values threshold limited" message is printed instead of opening a modal dialog, which blocked headless runs forever.

Pitfalls found while scripting the simulator:
- The run ID is the basename of the `--save` directory, so the path must not end in `/`.
- `US_Settings::tmpDir()` must exist. On a fresh account it does not, and the time state is then not written. The acceleration profile becomes garbage and the simulation runs out of memory. `svbench` creates a private work tree for each task.

## Assumptions to check

- In step 4 of the old workflow only N1 is loaded (`analysis.old_step3_noise = "step1"`). The alternative `"mbbest"` loads the TI+RI noise of the best meniscus/bottom model instead.
- The simulator binary is built from the branch. Its ASTFEM change (closed-form fixed-mesh stiffness) is meant to be numerically equivalent to main.
