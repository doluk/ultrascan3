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
  manifest.json            every case and the simulator runs it needs
  cases/<case>.json        one case definition, also the fit's input
  inputs/<case>/           model / buffer / simparams XML per data set
  data/<case>-dsNN/        the .auc us_astfem_sim wrote
  results/<case>.json      machine-readable fit result
  results/<case>.log       full fit output
  results/<case>.model.xml the fitted distribution
```

The `.auc` file name depends on the optical type, cell, channel and wavelength
`us_astfem_sim` settles on, so the driver discovers the file that was actually
written and patches the case file before fitting rather than predicting the
name.

## The cases

Twenty-four cases. Every species is declared in standard (20W) space;
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

Cases 20 and 21 pass by being **refused**: neither series carries enough
buoyancy contrast to determine v̄, and a fit that ran anyway would be reporting
a grid artefact. They are there to keep the gate honest.

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
prevent.

## Caveats

**D₂O density and viscosity are interpolated linearly** between pure H₂O and
pure D₂O. That is not how real D₂O mixtures behave, but the same numbers are
handed to the simulator and to the fit, so the departure cancels and the
harness still measures what it is meant to. Real analyses must use measured
buffer values.

**H/D exchange is not modelled.** In a real D₂O series the analyte's own mass
and v̄ shift as labile hydrogens exchange, so the truth the harness recovers is
cleaner than a real experiment's. See §3.5 of the design document.
