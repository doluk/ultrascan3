# ABDE in CsCl: parameter recovery when the gradient is right

A self-consistency check for 3DSA on an analytical buoyant density
equilibrium (ABDE) run. The data are simulated by `us_3dsa_cli simulate` with
the same solver and the same gradient the fit uses. The fit then has only the
solutes to find, so these cases show what the experiment determines when the
gradient model is right. They also show what the fit looks like when the
gradient is wrong.

The run is modelled on run 2098 (`doc/develop/3dsa_design.md` §3E):

* **Buffer.** PBS (1.00552 g/mL, 1.0117 cP), loaded at 1.40 g/mL CsCl
  (3.105 M by the component database's polynomial).
* **Rotor and cell.** 48 krpm, 20 °C, meniscus 6.05 cm, bottom 7.2 cm.
* **Scans.** 14 scans over 9.5 h. The first is at 41 min, after a 400 rpm/s
  ramp.
* **Noise.** 0.003 OD random noise.

CsCl is simulated by the ASTFVM as one solute whose *s* and *D* follow the
local density and viscosity. Its *s*₂₀,w of 0.447 S and *D*₂₀,w of
2.0 × 10⁻⁵ cm²/s give, at the loading concentration, the effective *s*/*D* that
puts the equilibrium gradient at ω²*r*/β with β = 1.25 × 10⁹. The gradient
spans 1.329 to 1.483 g/mL at 9.5 h.

| file | what it is |
|---|---|
| `simparams.xml` | the run: ASTFVM, 200 points, 0.002 cm data grid |
| `cscl_buffer.xml` | PBS plus CsCl, the gradient both simulation and fit use |
| `cscl_fast_buffer.xml` | the same CsCl with *s* and *D* doubled: the same equilibrium, formed twice as fast |
| `aav_ongrid.json` | empty capsids (65 S, *f/f₀* 1.2, v̄ 0.7440 = 1.3441 g/mL, 0.025 OD) and full (95 S, 1.2, 0.7146 = 1.3994 g/mL, 0.040 OD), each truth on a grid point. The bands peak near 0.5 and 1 OD |
| `aav_offgrid.json` | empty 69 S, 1.23, 0.74375; full 91 S, 1.16, 0.71485: every truth between grid points |
| `aav_fastformer.json` | the `aav_ongrid` data, fitted in the faster-forming gradient |

The grid is *s* 55–105 S in steps of 10, *f/f₀* 1.0–1.4 in steps of 0.1, and
v̄ in steps of 0.0005 over five values around each band: 300 points.

## Running

The cases write `data/` and `results/` beside themselves, so copy them out of
the source tree first:

```
CLI=$PWD/build/hpc/bin/us_3dsa_cli
cp -r test/3dsa_harness/abde_cscl /tmp/abde_cscl && cd /tmp/abde_cscl && mkdir -p results
$CLI simulate --config aav_ongrid.json --seed 11
$CLI fit --config aav_ongrid.json --out results/ongrid.json --curves results/ongrid.csv
$CLI simulate --config aav_offgrid.json --seed 12
$CLI fit --config aav_offgrid.json --out results/offgrid.json --curves results/offgrid.csv
$CLI fit --config aav_fastformer.json --out results/fast.json --curves results/fast.csv
```

Each fit takes about 36 minutes on four cores. A solve takes about 15 s, and
the 300 points form two subgrids, so two threads do the work.

## Results

Band values are signal-weighted over the solutes in each band. Measured with
the inputs here, seeds 11 and 12.

| case | RMSD | empty: *s*, *f/f₀*, v̄ | full: *s*, *f/f₀*, v̄ |
|---|---|---|---|
| `aav_ongrid` | 2.99 × 10⁻³ | 65.0 S, 1.20, 0.74400 | 95.1 S, 1.20, 0.71460 |
| `aav_offgrid` | 3.33 × 10⁻³ | 69.4 S, 1.27, 0.74375 (truth 69, 1.23, 0.74375) | 91.3 S, 1.20, 0.71485 (truth 91, 1.16, 0.71485) |
| `aav_fastformer` | 6.59 × 10⁻² | 55 S, 1.00, 0.7450 (grid edges) | 67 S, 1.40 (grid edge), 0.71458 |

With the gradient right, all 14 scans determine *s*, *f/f₀* and the density.
Off the grid, *f/f₀* comes within a third of a grid step. A gradient that
reaches the same equilibrium on the wrong time course leaves the residual far
above the noise. It also sends the solutes to the grid's edges and *s* off by
tens of S, which is what the all-scans fit of run 2098 shows.

These results were measured while the solver still added the magnitude of
CsCl's viscosity increment, where the database polynomial lowers the viscosity
(design doc §3E, limit 13, since fixed). Both sides used that model, so the
conclusions stand. With the fix, the CsCl gradient at 9.5 h moves by at most
0.0005 g/mL.

## us_abde on the same data

`simulate --density-csv density.csv` also writes the simulated gradient's
density at every scan and radius. `us_abde` converts the last scan's band
positions to densities with an ideal-gradient equation, by default CsCl at
M 168.36 g/mol and v̄ 0.2661. Applied to `aav_ongrid`'s last scan, that gives:

* **Empty band.** 1.2657 g/mL against 1.3441.
* **Full band.** 1.3884 g/mL against 1.3994.

The defaults' gradient is 2.5 times steeper than the simulated one. With the
buoyant molar mass matched to it (49 g/mol instead of 123), the same equation
gives 1.3428 and 1.3990. See the design doc §3E for the details.

