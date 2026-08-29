# 3DSA simulation and fit harness

Generates synthetic buoyancy-contrast series with `us_astfem_sim`, fits each
one with `us_3dsa_cli`, and reports the global RMSD together with the RMSD of
every data set.

See `doc/develop/3dsa_design.md` for what 3DSA is and why it needs a series of
runs in buffers of differing density.

## Building

`us_3dsa_cli` carries no GUI dependency and builds in both profiles:

```
cmake -S . -B build/app --preset <your app preset>     # or
cmake -S . -B build/hpc -DUS3_PROFILE=HPC -DUSE_QT6=ON
cmake --build build/hpc --target us_3dsa_cli
```

`us_astfem_sim` is part of the normal application build and needs the full Qt
and Qwt stack.

## Running

```
./test/3dsa_harness/run_harness.py --outdir /tmp/3dsa --bindir build/app/bin
```

Useful options:

| option | effect |
|---|---|
| `--only 'case1*'` | run a subset |
| `--jobs N` | fit N cases concurrently |
| `--threads N` | worker threads inside each fit |
| `--skip-gen` | reuse an existing case tree |
| `--skip-sim` | reuse existing `.auc` data and only re-fit |
| `--us-astfem-sim PATH`, `--us-3dsa-cli PATH` | explicit binary paths |

The driver exits non-zero if any case fails its expectation.

`us_astfem_sim` links the Qt GUI stack even when driven headlessly, so the
driver sets `QT_QPA_PLATFORM=offscreen` for it. No display is needed.

## What it produces

```
<outdir>/
  manifest.json               every case and the simulator runs it needs
  cases/<case>.json           one case definition, also the fit's input
  inputs/<case>/              model / buffer / simparams XML per data set
  data/<case>-dsNN/           the .auc us_astfem_sim wrote, plus the
                              experiment and edit XML it registered with it
  us3home/ultrascan/data/     analytes, buffers and solutions, as though the
                              run had come through us_convert and us_edit
  results/<case>.json         machine-readable fit result
  results/<case>.analysis.json  what a 2DSA cross-check of this case needs
  results/<case>.log          full fit output
  results/<case>.model.xml    the fitted distribution
```

The `.auc` file name depends on the optical type, cell, channel and wavelength
`us_astfem_sim` settles on, so the driver discovers the file that was actually
written and patches the case file before fitting rather than predicting the
name.

## The UltraScan work tree

`us_astfem_sim` does not only write the `.auc`. Alongside it, it writes an
experiment XML and an edit XML, and it registers a buffer, one analyte per
species and a solution tying them together, under `$HOME/ultrascan/data` — so
the run loads exactly as it would after `us_convert` and `us_edit`.

The driver points `HOME` at `<outdir>/us3home` (override with `--us3home`), so
a harness run neither reads nor disturbs a real UltraScan installation, and
each run starts from a clean store. To open a harness run in the GUI tools,
point them at that work tree.

## Preparing a run for analysis

Two things about what the simulator registers have to change before the run is
usable as a test. The driver does both, per data set, immediately after the
simulation.

**The edit profile's radial range.** `us_astfem_sim` writes
`data_range left = meniscus + 0.0005 cm`, which is inside the rounding of the
radial grid. `US_AstfemMath`'s interpolation calls `exit(-3)` outright — not an
error return, the process dies — when the simulation grid does not reach the
first edited radius. The driver rewrites the range to
`[meniscus + edit_margin, bottom - edit_bottom_margin]`, the same window
`us_3dsa_cli` trims to (0.02 and 0.10 cm), and moves the baseline and plateau
radii inside it. Both analyses then see exactly the same radii, which is the
only way their results are comparable.

**The analytes' partial specific volume.** The analytes carry the v̄ the data
was generated from. Handing those to a 2DSA analysis would give away the
answer 3DSA is being asked to find, and would leave `us_density_match` nothing
to recover. So the driver rewrites them to a decoy: the generator picks a value
0.05 mL/g from the concentration-weighted truth, in whichever direction leaves
more room inside 0.62–0.83 mL/g. Both records are rewritten — the solution
keeps a copy per analyte plus a `commonVbar20`, and each analyte file has its
own, and a reader may take either.

## Cross-checking against us_density_match

`us_2dsa` and `us_density_match` are GUI programs and the harness does not
drive them. What it does is leave every run in a state where they will load it,
and write `results/<case>.analysis.json` saying where everything is: the work
tree to point UltraScan at, each run's edit profile and the radial range now in
it, the solution and analyte records now carrying the decoy, and the truth
those records no longer mention.

The intended comparison is: run `us_2dsa` on each cell of a series — which will
use the decoy v̄, so its *s* and *D* are wrong in a way that depends on the
buffer — then `us_density_match` across the series to recover v̄ from how the
apparent values move with buffer density. That number is what `us_3dsa_cli`
reports directly as `vbar_fit`, and `truth_vbar` in the same file is what both
should land on.

## The cases

Thirty-one cases. Every species is declared in standard (20W) space;
`us_astfem_sim` converts to experimental space per component using that
component's own v̄ and the buffer it is given, and the fit has to invert that.
That round trip is the point.

| # | case | what it exercises |
|---|---|---|
| 1 | `single_2buffers` | minimum series: H₂O and 100 % D₂O |
| 2 | `single_3buffers` | 0 / 50 / 100 % D₂O |
| 3 | `single_5buffers` | five isotope concentrations |
| 4 | `single_unequal_loading` | loadings 1.0 / 0.7 / 1.4 |
| 5 | `single_extreme_loading` | loadings 1.0 / 0.4 / 2.0 |
| 6 | `two_species_same_vbar_diff_s` | mixture, shared v̄, s of 4.0 and 6.5 S |
| 7 | `two_species_same_vbar_diff_s_and_D` | shared v̄, different s **and** D |
| 8 | `two_species_same_s_diff_D` | shared v̄ and s, f/f₀ of 1.5 and 2.5 |
| 9 | `two_species_diff_vbar_same_s` | v̄ of 0.73 and 0.69 at the same s |
| 10 | `two_species_diff_vbar_diff_s` | s, D and v̄ all differ |
| 11 | `three_species_same_vbar` | three-component mixture, shared v̄ |
| 12 | `three_species_mixed_vbar_5buffers` | two share v̄, one differs |
| 13 | `random_noise_low` | random noise 0.001 OD |
| 14 | `random_noise_high` | random noise 0.005 OD |
| 15 | `ti_noise` | time-invariant noise, TI fitted |
| 16 | `ri_noise` | radially-invariant noise, RI fitted |
| 17 | `all_noise` | random + TI + RI together |
| 18 | `mixture_all_noise` | mixture with all three noise sources |
| 19 | `mixture_noise_unequal_loading_5buffers` | everything at once |
| 20 | `weak_contrast_refused` | 0 and 15 % D₂O — **must be refused** |
| 21 | `single_dataset_refused` | one data set — **must be refused** |
| 22 | `low_vbar` | v̄ = 0.65, the low grid edge |
| 23 | `high_vbar` | v̄ = 0.80, the high grid edge |
| 24 | `wide_s_range_mixed_vbar` | 1.5 S and 8.0 S with different v̄ |
| 25 | `random_noise_per_dataset` | random noise 0.001 / 0.003 / 0.006 OD **by cell** |
| 26 | `random_noise_per_dataset_mixture` | mixture, 0/30/70 % D₂O, noisiest cell first |
| 27 | `noise_per_dataset_unequal_loading` | noise *and* loading vary by cell, 0/20/45/80 % |
| 28 | `ti_ri_per_dataset` | TI and RI at four different levels, one per cell |
| 29 | `series_0_30_50` | 0/30/50 % D₂O — no pure-D₂O cell |
| 30 | `series_0_20_40_60` | 0/20/40/60 % D₂O, mixture — half the usual contrast |
| 31 | `series_uneven_5_steps` | 10/35/55/70/95 % D₂O — unevenly spaced, no H₂O cell |

Cases 20 and 21 pass by being **refused**: neither series carries enough
buoyancy contrast to determine v̄, and a fit that ran anyway would be reporting
a grid artefact. They are there to keep the gate honest.

### Noise that differs from cell to cell

Cases 25–28 give every cell of a series a *different* noise level, which is
what a real series has: each cell is loaded, scanned and read separately.

That makes the two aggregate checks meaningless — a 6× spread in per-data-set
RMSD is now the correct answer, not a defect — so those cases switch them off
and cap each data set separately instead, at roughly twice the random noise
that cell was given. The check is that every cell's residual reaches the level
*it* was given, neither better nor worse:

```
case25_random_noise_per_dataset                 3    3.9280e-03    0.7291  0.7300  PASS
     ds00 (0% D2O)    RMSD 1.0107e-03  scale 1.0000  noise 0.0010  cap 0.0020 ok
     ds01 (50% D2O)   RMSD 3.0156e-03  scale 1.0000  noise 0.0030  cap 0.0050 ok
     ds02 (100% D2O)  RMSD 6.0143e-03  scale 1.0001  noise 0.0060  cap 0.0100 ok
```

Case 27 is the awkward one on purpose: the noisiest cell is also the most
weakly loaded, so noise and loading pull the amplitude factors in opposite
directions. Case 28 asks the same of fitted noise — four cells, four different
TI/RI pairs, each cell's residual expected to fall to its own random floor.

### Series that are not 0/50/100

Cases 29–31 step the D₂O unevenly and do not run the full range: no pure-D₂O
cell, no H₂O cell, uneven spacing. Real series rarely are tidy — stock runs
out, a cell is lost, and the top of the range costs the most H/D exchange. The
contrast gate and the v̄ recovery have to depend on the densities themselves,
not on the series being regular.

Cases 15–19 are the reason `US_SolveSimMDS` exists. Each cell of a real series
has its own time- and radially-invariant noise, so these cases inject a
different profile into every data set and ask the fit to solve for it. Beyond
the global RMSD they are checked on `rmsd_spread_max`, the ratio of the worst
per-data-set RMSD to the best: a fit that cleans up one data set and leaves
the rest passes every aggregate check but fails that one. See §3C of the
design document.

## Reading the report

Per case the driver prints the global RMSD, the fitted and true
concentration-weighted v̄, and then one line per data set:

```
case04_single_unequal_loading                   3    7.1e-05    0.7298   0.7300  -0.0002   PASS
     ds00 (0 pct D2O)      rho 0.9982  RMSD 5.8e-05  scale 1.0000 (true 1.0000)
     ds01 (50 pct D2O)     rho 1.0517  RMSD 7.9e-05  scale 0.7001 (true 0.7000)
     ds02 (100 pct D2O)    rho 1.1050  RMSD 7.4e-05  scale 1.3998 (true 1.4000)
```

`scale` is the fitted per-data-set amplitude against `scale true`, the loading
ratio the data was generated with. A series where one data set's RMSD is far
above the others is the signal that the fit is trading that data set off
against the rest, which is exactly what the amplitude factors exist to
prevent — unless the noise itself differs by cell, in which case the line
carries that cell's own injected level and cap instead (see below).

## Caveats

**D₂O density and viscosity are interpolated linearly** between pure H₂O and
pure D₂O. That is not how real D₂O mixtures behave, but the same numbers are
handed to the simulator and to the fit, so the departure cancels and the
harness still measures what it is meant to. Real analyses must use measured
buffer values.

**Thirty scans per run.** Twelve resolved a boundary but left the fit short of
the time information that separates *s* from *D*, and the noise cases in
particular were being asked to separate systematic noise from the model on a
dozen time points. Raising it to thirty cut case 16's residual four-fold and
case 18's v̄ error five-fold. A real velocity run collects far more.

**H/D exchange is not modelled.** In a real D₂O series the analyte's own mass
and v̄ shift as labile hydrogens exchange, so the truth the harness recovers is
cleaner than a real experiment's. See §3.5 of the design document.
