# Fitting the meniscus and the radial scan time (tau) with us_mpi_analysis

Absorbance scanners record the radial points of a scan one after another.
The stored scan time is the time of the first recorded point; the point at
radius *r* is recorded

    t(r) = t_scan + |r − r_start| / Δr · τ

later, with τ the time per radial point (s/point), Δr the mean radial spacing
of the data and r_start the radius where the scan starts. A τ > 0 that the
model ignores biases the fit, most of all D, f/f0 and MW, and a scan start
outside the meniscus adds a constant delay that shifts the fitted meniscus
(see the simulations in the commit history of this branch).

`us_mpi_analysis` can now

* use a **fixed τ** for any analysis (2DSA, GA, DMGA, PCSA), and
* **fit τ in a 2DSA**, on a grid of τ values, alone or together with the
  meniscus (and/or bottom) grid, just like the meniscus + bottom fit.

## Job parameters (`<jobParameters>` of the hpcrequest XML)

| Parameter | Default | Meaning |
|---|---|---|
| `radial_scan_time` | 0 | Fixed τ in s/point (all methods). 0: all points of a scan at the scan time, as before. |
| `scan_start` | 0 | Radius (cm) where each scan starts. 0: the first raw radius of the `.auc` file in scan direction (the smallest radius for outward scans, the largest for inward scans). |
| `scan_direction` | `outward` | `outward` (towards the bottom) or `inward`. |
| `tau_points` | 0 | Number of τ values; > 1 fits τ (2DSA only). |
| `tau_min`, `tau_max` | 0 | Range of the fitted τ values (s/point); `tau_max` is required with `tau_points` > 1. |

The fit runs one 2DSA per grid point: `meniscus_points × tau_points` (times
`bottom_points` for a meniscus + bottom fit). The meniscus/bottom points vary
fastest. The τ fit cannot be combined with Monte Carlo or global fits.
Jobs without these parameters give exactly the results they gave before.

Output: one model (and noise) per grid point, with τ in the iteration ID of
the file name and description, e.g. `...2DSA-FM_000001_i03-m59000t04000.model`
(meniscus 5.9000 cm, τ = 0.4000 s/point; a τ-only fit gives `i05-t04000`).
`analysis_files.txt` in `analysis-results.tar` lists every point with
`meniscus_value`, `tau_value` and `variance`.

## Workflow on the cluster

1. **Build** `us_mpi_analysis` from this branch as usual (HPC profile, or
   `-DUS3_NO_DB=ON -DUS3_BUILD_PROGRAMS=ON` with MPI).

2. **Make a job archive** from the data files on disk:

   ```bash
   programs/us_mpi_analysis/tau_fit/make_tau_job.py \
       --auc run.RA.1.A.280.auc --edit run.e1.RA.1.A.280.xml \
       --timestate run.time_state.tmst \
       --density 0.99823 --viscosity 1.002 --vbar 0.733 \
       --centerpiece-bottom 7.2 --rotor-stretch "<c1> <c2>" \
       --s-min 1 --s-max 10 --s-points 64 --ff0-min 1 --ff0-max 4 --ff0-points 64 \
       --uniform-grid 6 --ti-noise --ri-noise \
       --meniscus-range 0.02 --meniscus-points 11 \
       --tau-min 0 --tau-max 0.10 --tau-points 11 \
       -o run_tau.tar
   ```

   * The **rotor stretch** coefficients and **centerpiece bottom** must be
     those of your rotor calibration and centerpiece (UltraScan LIMS: rotor
     calibration, centerpiece channel bottom); the LIMS default 7.3 is rejected.
   * `--band-volume <ml>` for band-forming centerpieces (default 0: boundary).
   * `--scan-start` / `--scan-direction` if the default (first raw radius,
     outward) is not right for your instrument. For XL-A/XL-I data the raw
     radii of the `.auc` start where the scanner started. For Optima data,
     give the start radius and direction of the scan explicitly if the
     exported radial range does not begin at the scan start.
   * Repeat `--auc/--edit` for several cells/triples: they are fitted one
     after the other with the same grid.
   * Choose the τ range from the instrument: τ ≈ scan duration / number of
     radial points. Include τ = 0 as control.

3. **Run** it, e.g. with Slurm:

   ```bash
   #!/bin/bash
   #SBATCH --job-name=tau_fit
   #SBATCH --ntasks=32
   #SBATCH --time=04:00:00
   module load openmpi            # as on your cluster
   mkdir -p work && cd work && cp ../run_tau.tar .
   srun us_mpi_analysis -walltime 230 -mgroupcount 1 run_tau.tar
   # results: work/output/analysis-results.tar
   ```

   (`mpirun -np 32 us_mpi_analysis -walltime 230 -mgroupcount 1 run_tau.tar`
   works the same way.) The run time is about `meniscus_points × tau_points`
   times that of a single 2DSA.

4. **Evaluate**:

   ```bash
   programs/us_mpi_analysis/tau_fit/tau_landscape.py work/output/analysis-results.tar \
       --sigma 0.004 --points <scans x radial points> --plot
   ```

   It prints the RMSD landscape over meniscus × τ, the grid minimum, the
   optimum refined by parabolas through the τ and meniscus profiles, profile
   1σ uncertainties (Δχ² = 1; give the noise σ, e.g. the RMSD of a good fit,
   and the number of fitted points), and the weighted s, f/f0 and MW at the
   minimum compared with τ = 0 at the same meniscus. `--plot` writes
   `<triple>_tau_landscape.png`.

   For a final model, rerun the 2DSA (with refinement iterations or Monte
   Carlo) with the fitted meniscus in the edit and `radial_scan_time` set to
   the fitted τ (`make_tau_job.py --radial-scan-time <tau>`).

## Notes

* τ is defined per point of the mean radial spacing of the *edited* data,
  Δr = (r_last − r_first)/(n − 1). For data at 0.001 cm, τ = 0.06 s/point
  is 60 s/cm.
* τ is well determined when the scan delay is large compared with the
  noise: many radial points, sharp boundaries or bands, low noise. On sparse
  data (e.g. 0.006 cm spacing, 60 s scans) the τ profile is shallow and the
  uncertainty large; check the profile before using the value. Fitted TI/RI
  noise absorbs part of the τ signature of boundary runs (the plateau tilt),
  not of band-forming runs.
* The LIMS and `us_fit_meniscus` do not know the τ suffix of the model
  names; use `tau_landscape.py` for the evaluation.
