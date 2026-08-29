# 3DSA — Simultaneous fitting of *s*, *D* and *v̄*

**Design and implementation plan**

Status: proposal / conceptual design
Scope: new analysis program `us_3dsa`, supporting `utils/` changes, MPI back end, validation plan

---

## 1. Executive summary

2DSA fits a two-dimensional grid of (*s*<sub>20,W</sub>, *f/f₀*) with **v̄ held fixed**, and reports
*D* as a derived coefficient. The request is a "3DSA" that fits *s*, *D* **and** v̄ at the same time.

The central finding of this analysis is:

> **A three-dimensional (*s*, *f/f₀*, v̄) grid is exactly rank-deficient for a single sedimentation
> velocity dataset.** The Lamm-equation forward model consumes only the experimental-space pair
> (*s\**, *D\**); v̄ enters solely through the standard-space ↔ experimental-space corrections. A map
> from three grid coordinates onto two observable coordinates has one-dimensional fibres, so an
> infinite family of (*s*, *f/f₀*, v̄) triples produces the *identical* NNLS column. NNLS would return
> an arbitrary point on that ridge, and the reported v̄ would be meaningless.

Therefore 3DSA is designed here as a **global, multi-dataset analysis over a buoyancy-contrast
series** (a density series: H₂O / D₂O / H₂¹⁸O / sucrose / Nycodenz / glycerol). Across buffers of
different density ρ, a single species (*M*, *f*, v̄) sediments at different rates, and the *pattern*
of that shift across the series is what determines v̄. This is the same physics that
`us_density_match` already exploits *post hoc*; 3DSA does it *simultaneously*, inside the NNLS, so
that concentrations, *s*, *D* and v̄ come out of one consistent fit instead of a boundary-fraction
matching heuristic.

The good news for implementation: **the core solver already supports this.**
`US_SolveSim::calc_residuals()` already (a) stacks rows across multiple datasets into one design
matrix, and (b) has a code path that reads three independent attributes off each solute and
recomputes the buffer correction *per dataset from the solute's own v̄*. The work is therefore
concentrated in grid generation, scheduling bookkeeping, GUI, MPI plumbing, and — importantly —
two correctness fixes and one new outer loop, all identified in §5 and §6.

**Recommended shape of the deliverable**

| | |
|---|---|
| New desktop program | `programs/us_3dsa/` (mirrors `us_2dsa`, adds a dataset-series manager) |
| New shared code | 3-D grid generation + identifiability metric in `utils/` |
| Solver changes | one bug fix + one flag in `US_SolveSim`; no algorithmic rewrite |
| New solver feature | per-dataset amplitude scale factors (outer loop) |
| MPI | `3dsa_master.cpp` / `3dsa_worker.cpp`, parser and model-writer branches |
| External dependency | LIMS/job-submission side must learn the `3DSA` method |
| Estimated effort | ≈ 2–2.5 developer-months, plus LIMS work |

---

## 2. What 2DSA does today

Grounding the design in the current code:

1. **Grid.** `US_Solute::init_solutes()` (`utils/us_solute.cpp:15`) builds a rectangular
   (*s*, *f/f₀*) grid and partitions it into `grid_reps²` interleaved subgrids. Defaults from the
   GUI are 64 × 64 points with 8 repetitions → 64 subgrids of 64 points
   (`programs/us_2dsa/us_analysis_control_2d.cpp:116-118`).
2. **Subgrid count.** `nsubgrid = sq( ngrefine )` (`programs/us_2dsa/us_2dsa_process.cpp:170`).
3. **One column per grid point.** `US_SolveSim::calc_residuals()` builds a single-component model
   for each solute, calls `update_coefficients()` to fill in the missing coefficients, converts to
   experimental space, runs the ASTFEM Lamm solver, and writes the result into one column of the
   NNLS `A` matrix.
4. **Rows stack across datasets.** The `b` vector is filled by looping
   `for ( ee = offset; ee < lim_offs; ee++ )` over datasets
   (`utils/us_solve_sim.cpp:280-311`), and each `A` column is filled the same way. A global fit is
   therefore already a single tall NNLS problem.
5. **Depth refinement is dimension-agnostic.** Contrary to first appearances,
   `US_2dsaProcess::process_job()` (`programs/us_2dsa/us_2dsa_process.cpp:1071`) does **not**
   geometrically subdivide the grid at depth > 0. It merely accumulates the solutes that survived
   NNLS from completed subgrids and re-runs NNLS on their union until one final pass remains. That
   logic carries over to three dimensions **unchanged**.
6. **A generalised attribute path already exists.** `US_SolveSim` supports a 9-bit "solute type"
   mask `(attr_x << 6) | (attr_y << 3) | attr_z` with
   `ATTR_S=0, ATTR_K=1, ATTR_W=2, ATTR_V=3, ATTR_D=4, ATTR_F=5`. When `stype > 9`
   (`utils/us_solve_sim.cpp:645`) it sets **three** attributes per solute and recomputes
   `US_Math2::data_correction()` **per dataset** from that solute's v̄. This is exactly the
   machinery 3DSA needs.
7. **`US_Solute` already carries the fields.** `s`, `k`, `c`, `v`, `d` — so a solute can hold
   (*s*, *f/f₀*, v̄) or (*s*, *D*, v̄) with no change to the struct, and MPI already ships all five
   doubles (`solute_doubles`).

---

## 3. Why 3DSA is not a trivial extension: identifiability

### 3.1 The forward model only sees (*s\**, *D\**)

`US_Astfem_RSA` receives, per component, exactly three numbers: `s`, `D`, and `extinction`
(`utils/us_astfem_rsa.cpp:459-461` and `:732-734`). `vbar20` is referenced nowhere in the solver
except a debug print (`utils/us_astfem_rsa.cpp:4249`). Buffer compressibility is likewise inert in
the present engine. So:

$$\text{column}_j = \mathcal{L}\big(s^*_j,\; D^*_j\big)$$

### 3.2 The correction that carries v̄

From `US_Math2::data_correction()` (`utils/us_math2.cpp:699-705`), with
`adjust_vbar20()` currently the identity (`utils/us_math2.h:166-168`, temperature adjustment
disabled), for a dataset *e* at temperature *T<sub>e</sub>*, buffer density ρ<sub>e</sub>, buffer
viscosity η<sub>e</sub>:

$$
s^*_e = s_{20,W}\cdot\underbrace{\frac{1-\bar v\rho_e}{1-\bar v\rho_{20,W}}}_{B_e(\bar v)}\cdot\frac{\eta_{20,W}}{\eta_e},
\qquad
D^*_e = D_{20,W}\cdot\frac{T_e}{T_{20}}\cdot\frac{\eta_{20,W}}{\eta_e}
$$

**v̄ appears in *s\** and not in *D\**.** That asymmetry is the entire information channel.

### 3.3 The degeneracy, stated precisely

For one dataset the grid map is

$$\varphi:\ (s_{20,W},\, f/f_0,\, \bar v)\ \in\ \mathbb{R}^3 \ \longrightarrow\ (s^*,\, D^*)\ \in\ \mathbb{R}^2$$

a smooth surjection whose generic fibres are one-dimensional curves. Every point on such a curve
gives a **bit-identical** NNLS column. Consequences:

* The design matrix has near-duplicate columns; the normal equations are singular to working
  precision.
* NNLS still returns *a* solution — the active-set rule picks a representative deterministically by
  column order — but the v̄ coordinate of that representative is an artefact of grid ordering, not a
  measurement.
* No amount of regularisation recovers the lost dimension; Tikhonov merely picks the
  minimum-norm point on the ridge.

**This must be a hard gate in the program, not a footnote in the manual.**

### 3.4 What breaks the degeneracy: buoyancy contrast

With *N* ≥ 2 datasets at different densities, the stacked column for a grid point is
(*s\**₁, *D\**₁, *s\**₂, *D\**₂, …). Two grid points now have to agree in ≥ 3 independent quantities
to collide, and generically only the trivial solution remains. The observable that carries v̄ is the
ratio

$$R(\bar v)=\frac{B_2(\bar v)}{B_1(\bar v)}=\frac{1-\bar v\rho_2}{1-\bar v\rho_1},
\qquad
\frac{\partial \ln R}{\partial \bar v}=\frac{\rho_1}{1-\bar v\rho_1}-\frac{\rho_2}{1-\bar v\rho_2}$$

Evaluated at v̄ = 0.73 mL/g:

| ρ₂ (g/mL) | contrast | ∂ln*R*/∂v̄ (mL/g)⁻¹ | δv̄ for 1 % precision on *s*-ratio |
|---|---|---|---|
| 1.1050 (100 % D₂O) | full | −2.04 | **0.005** |
| 1.0500 (≈ 50 % D₂O) | half | −0.82 | 0.012 |
| 1.0100 | weak | −0.17 | 0.060 — useless |

against ρ₁ = 0.9982 (H₂O at 20 °C).

Two conclusions fall straight out of that table, and both feed directly into the design:

* **Design decision D6 — the identifiability gate.** Compute |∂ln*R*/∂v̄| over the loaded series
  before allowing a fit; refuse below ≈ 0.5 (mL/g)⁻¹ and warn below ≈ 1.0.
* **Design decision D-grid — the v̄ axis must be coarse.** Achievable v̄ resolution is
  0.005–0.01 mL/g at best. A v̄ range of 0.60–0.85 therefore needs **25–50 grid points, not 64**,
  and 16–20 is a sensible default. This single observation is what keeps the cubic cost tractable
  (§7).

### 3.5 Two physics caveats that must be surfaced in the UI

1. **H/D exchange.** In a D₂O series the analyte's own mass and v̄ change through exchange of
   labile hydrogens — the very quantity being fitted is perturbed by the contrast agent. This is
   why `us_density_match` tracks `d2opct` per distribution. 3DSA should (a) prefer non-exchanging
   density modifiers (H₂¹⁸O, sucrose, Nycodenz, glycerol) in its documentation, and (b) carry a
   per-dataset D₂O-percent field with a hook for a mass/v̄ correction (deferred to a later phase,
   but the field belongs in the data model from day one).
2. **Buffer density and viscosity accuracy.** v̄ is inferred from a *ratio of buoyancy terms*; a
   systematic error in ρ or η propagates directly into v̄ with the gain in the table above. The
   dataset-series panel must show ρ, η, *T* per dataset and where each came from (measured vs.
   computed from the buffer composition).

### 3.6 The per-dataset amplitude problem

`US_SolveSim` gives each solute **one** NNLS coefficient that multiplies its stacked column across
*all* datasets — i.e. it assumes identical signal concentration in every dataset. For a density
series, each experiment is a separate cell loading, so that assumption is generally wrong and the
resulting bias lands in v̄.

Note the RI-noise machinery does *not* solve this: RI noise is an additive per-scan offset, not a
multiplicative per-dataset scale.

Fortunately the primary v̄ signal is the **boundary-position shift**, which is scale-invariant.
Fitting a per-dataset scale factor therefore costs almost no information about v̄ while removing a
real bias. See design decision D5 and algorithm §6.4.

---

## 3A. Phase 0 results — the analysis, measured

Phase 0 has been run. `test/utils/test_us_solve_sim_vbar.cpp` drives the real
`US_Model::calc_coefficients()`, `US_Math2::data_correction()` and
`US_Astfem_RSA::calculate()`; all six tests pass in ~125 ms. The numbers below
are the measured output, not estimates.

### The fibre is exact

Taking a reference species (4 S, *f/f₀* = 1.5, v̄ = 0.73) in water, and inverting
the corrections at each v̄ in 0.60–0.85 to recover the (*s*<sub>20,W</sub>, *f/f₀*)
that reproduces the same observables:

| v̄ (mL/g) | *s*<sub>20,W</sub> (S) | *f/f₀* | *M* (Da) | rel. Δ*s\** | rel. Δ*D\** |
|---|---|---|---|---|---|
| 0.600 | 3.9999 | 1.824 | 45,880 | 0 | 0 |
| 0.700 | 4.0000 | 1.575 | 61,084 | 0 | 0 |
| 0.730 | 4.0000 | 1.500 | 66,602 | 0 | 0 |
| 0.800 | 4.0001 | 1.317 | 91,358 | 0 | 0 |
| 0.850 | 4.0002 | 1.174 | 121,456 | 0 | 0 |

The deviation is not small, it is **exactly zero to machine precision**, and the
implied molar mass varies by a factor of **2.65** along the fibre. The
*s*<sub>20,W</sub> spread over the whole fibre is 0.008 %.

### The solver confirms it

The stronger statement, at the level of the Lamm solver rather than the
coefficient algebra. Two fibre points — v̄ = 0.60 (*M* = 45,880) and v̄ = 0.85
(*M* = 121,456) — simulated through `US_Astfem_RSA` over 5 scans × 701 radial
points:

```
max |A − B| = 0.000000e+00      max |A| = 1.191829e+02
```

**Bit-identical.** As a control, perturbing *D\** by 0.1 % in one of the two
simulations raises `max |A − B|` to 0.1155, so the test resolves differences
five orders of magnitude smaller than the effect it reports as absent.

### Contrast lifts it

Spread in *s\** across that same fibre, evaluated in each buffer:

| Buffer | ρ (g/mL) | *s\** spread across the fibre |
|---|---|---|
| H₂O (the one it was built in) | 0.9982 | 0.000 % |
| 100 % D₂O | 1.1050 | **109.6 %** |
| ≈ 50 % D₂O | 1.0500 | 30.0 % |
| Weak contrast | 1.0100 | 5.2 % |

### The sensitivity table is confirmed

Closed form versus a central difference of the actual corrections:

| Second buffer | closed form | numeric | §3.4 table | δv̄ at 1 % |
|---|---|---|---|---|
| 1.1050 | −2.0354 | −2.0354 | −2.04 | 0.0049 |
| 1.0500 | −0.8172 | −0.8172 | −0.82 | 0.0122 |
| 1.0100 | −0.1651 | −0.1651 | −0.17 | 0.0606 |

### One correction to the design

The gain must be evaluated at the **temperature-corrected** density
`SolutionData::density_tb`, not the nominal 20 °C buffer density.
`data_correction()` forms the buoyancy term from
`density_tb = density · density_wt(T) / DENS_20W`, and `density_wt(20)` is not
exactly the `DENS_20W` constant. Using the nominal density leaves a systematic
residual of 3.0 × 10⁻⁶ relative; using `density_tb` brings agreement to
7 × 10⁻¹¹, i.e. pure finite-difference noise.

At 20 °C this is negligible, but the factor `density_wt(T)/DENS_20W` departs
from 1 by roughly 0.1 % at 25 °C and 2 % at 40 °C — around 1.7 % on the gain at
25 °C — which is enough to move a series across the gate threshold. **Phase 1's
`buoyancy_contrast()` must take `density_tb` from `data_correction()`**, and the
test asserts that the nominal-density form is measurably worse.

### Exit criterion

Met. The single-dataset degeneracy is exact, the density-series separation is
large, and the §3.4 sensitivity table reproduces to the precision quoted. The
design proceeds unchanged apart from the `density_tb` correction above.

---

## 3B. Phase 2 results — the engine, measured

`utils/us_3dsa_process.{h,cpp}` is the fit engine, GUI-free so the desktop
program, the MPI back end and the test suite share it.
`test/utils/test_us_3dsa_process.cpp` drives it end to end over synthetic
buoyancy-contrast series; all seven tests pass in ~6 s.

### The exit criterion is met

Three datasets (ρ = 0.998, 1.050, 1.105) generated from one species at
*s* = 4 S, *f/f₀* = 1.5, v̄ = 0.73, with **deliberately different cell
loadings** of 1.00, 0.70 and 1.40. Grid 5 × 5 × 5 with the truth on a grid
point:

| quantity | true | recovered |
|---|---|---|
| v̄ | 0.7300 | **0.7299** |
| *s* (S) | 4.0000 | **3.9998** |
| loading ratios | 1.00 / 0.70 / 1.40 | **1.0000 / 0.7000 / 1.3999** |
| RMSD | — | 3.1 × 10⁻⁵ |

The recovered v̄ error of 1 × 10⁻⁴ mL/g sits well inside the 0.0049 mL/g the
contrast implies. Dropping the amplitude factors on the same data moves the
v̄ error to 0.078 and the RMSD to 0.30 — two thousand times worse — which is
the measurement behind design decision D5.

### The amplitude loop needed accelerating

Alternating NNLS with the closed-form amplitudes converges, but only
geometrically: measured contraction was a near-constant **0.79 per pass**,
needing ~26 passes over the whole grid to settle. Each pass is a full fit, so
that is not affordable.

A geometric sequence has a known limit, so the loop now takes it — Aitken/
Steffensen extrapolation in log space over three successive iterates, skipped
when the sequence is not actually contracting, and always verified by
re-fitting afterwards. Convergence went from >26 passes to **7**, and the
RMSD from 9.7 × 10⁻³ to 3.1 × 10⁻⁵.

### Two bugs found, both outside 3DSA

**`US_Math2::nnls` corrupts memory on ill-conditioned systems.** The
secondary loop of Lawson & Hanson removes coefficients from the active set one
at a time, re-checking feasibility after each removal. The feasibility flag
was set once *before* the loop instead of before each re-check, so it could
only ever go from 1 to 0: one round-off-induced removal makes the loop run
until the set-P counter and index cursor fall below zero and the routine
writes outside its index array. It never terminates cleanly once it enters
that path, so the fix can only change results that were already invalid.

This is the solver under **2DSA, PCSA, GA and DMGA**, not just 3DSA. A 3DSA
fit over two datasets — only marginally identifiable, hence badly conditioned
— hits it reliably; `TestUS3dsaProcess.ProducesAStandardSpaceModel` segfaults
without the fix. Twenty thousand randomly generated near-rank-deficient
systems did *not* reach it, so the trigger needs the particular structure of
real Lamm-equation columns. `test/utils/test_us_math2_nnls.cpp` adds the
general coverage the routine previously lacked entirely.

**`US_SolveSim::DataSet` had no default initialization.** Any caller that
forgot a scalar handed `calc_residuals` an indeterminate value — and
`solute_type` selects which branch of the fit runs. Scalars now carry default
initializers.

### The column cache does not work — D7 is withdrawn

§7 claimed a 3–10× saving from caching columns keyed on quantised
(*s\**, *D\**), reasoning that the 3-D grid maps onto a 2-D manifold so many
points must collide. **Measured over a production 64 × 64 × 16 grid, they do
not:**

| relative tolerance | distinct columns | of total | saving |
|---|---|---|---|
| 10⁻⁵ | 65,536 | 100.0 % | 1.00× |
| 10⁻⁴ | 64,332 | 98.2 % | 1.02× |
| 10⁻³ | 54,838 | 83.7 % | 1.20× |
| 10⁻² | 16,270 | 24.8 % | 4.03× |

The inference was wrong. The image of the grid being two-dimensional does not
make grid points *coincide* — the v̄ axis slides a point **along** the
manifold rather than onto a neighbour. What that confinement produces is
near-collinearity, not duplication: a **conditioning** problem, not a
**redundancy** one. (It is the same conditioning that broke the NNLS routine
above.) A cache only pays at 10⁻² relative, which is coarser than the data
noise and would corrupt the fit.

**Consequence for §7.** Without the cache the third axis costs what it looks
like it costs: *n*<sub>v</sub> × a 2DSA run per dataset — 16× at the default
— times the series length. The "2–5× per dataset" figure in §7.2 depended on
the cache and is withdrawn with it. Corrected in §7 below.

---

## 3C. What the harness found — the noise path was single-dataset

`test/3dsa_harness/` (§8.2) runs 24 synthetic cases through `us_astfem_sim`
and `us_3dsa_cli`. On the first run nineteen behaved as designed. The five
that fit noise exposed a defect that was not in 3DSA at all. All 24 pass now;
this section records what was wrong and what was done about it.

### The symptom

Case 15 injects 0.005 OD of time-invariant noise into each of three data sets
and asks the fit to solve for TI noise. Per-data-set residual RMSD:

| data set | ρ (g/mL) | RMSD |
|---|---|---|
| ds00 | 0.9982 | **7.6 × 10⁻⁶** |
| ds01 | 1.0516 | 4.97 × 10⁻³ |
| ds02 | 1.1050 | 5.04 × 10⁻³ |

The noise was removed from the **first** data set essentially perfectly, and
from the other two not at all — their residuals sit at exactly the injected
level. The harness reports this directly as a per-data-set RMSD spread of
**609×**.

Radially-invariant noise fails differently and worse. Case 16 injects 0.005 OD
of RI noise and asks for it to be fitted; the residual comes back at
**5.0 × 10⁻²** in every data set — an order of magnitude *above* the injected
level, so the fit is worse than not correcting at all. The spread there is
only 1.9×, which is why a check on aggregate RMSD alone would have caught case
16 but not case 15, and a spread check alone the reverse. The harness applies
both.

Cases 13 and 14, which inject *random* noise and do not ask for it to be
fitted, behave correctly: the residual lands at the injected level in every
data set, evenly, and the recovered v̄ is unaffected (error 0.0006 and 0.0011).

### The cause

All six helpers behind the noise algebra in `US_SolveSim::calc_residuals` read
`data_sets[ d_offs ]` — the first data set — and loop over *that* set's
`pointCount()` and `scanCount()`:

`compute_a_tilde`, `compute_L_tildes`, `compute_L_tilde`, `compute_a_bar`,
`compute_L_bars`, `ti_small_a_and_b`, `ri_small_a_and_b`.

The surrounding code sizes the noise vectors for the whole series —
`ntinois` and `nrinois` accumulate over every data set, and the residual loop
indexes them per data set with `tinoffs`/`rinoffs`. So the vectors have room
for the series, but only the first data set's block is ever written; the rest
stay zero and no noise is subtracted from them.

### Scope

This is **not** a 3DSA regression. It predates this work and affects any
multi-data-set fit that asks for TI or RI noise — including **global 2DSA**
through the MPI path, which is the configuration most likely to be hit in
production. A single-data-set fit is unaffected, which is why it has survived:
that is the overwhelmingly common case.

### The fix: `US_SolveSimMDS`

`US_SolveSim` is left alone. Making its noise algebra data-set aware would
change the numerical path of every 2DSA, PCSA, GA and DMGA run in service of a
case none of them hit today, in a routine GMP analyses depend on. So the
solver is copied to `utils/us_solve_sim_mds.{h,cpp}` as `US_SolveSimMDS`, and
only the copy is changed. `US_3dsaProcess::run_task()` constructs the copy;
nothing else does.

The copy shares `US_SolveSim::DataSet` and `US_SolveSim::Simulation` by
typedef rather than duplicating them, so the same data sets and the same
`Simulation` object can be handed to either class, and the static analysis
helpers (`checkGridSize`, `buoyancy_contrast`, `vbar_resolution`) are called
on `US_SolveSim` rather than restated.

What changes in the copy, and nothing else:

1. `calc_residuals` builds a small geometry table once — for each data set,
   its point and scan counts and its offset into three different vectors: the
   concatenated data (`toffs`), the TI-length vectors (`tioffs`) and the
   RI-length vectors (`rioffs`). The counting loop it replaces computed only
   the totals.
2. Every routine in the noise algebra loops over that table instead of reading
   `data_sets[ d_offs ]`. Each average is taken **within** a data set: "a~"
   over that set's radial points, "a-bar" over that set's scans.
3. The reduced normal equations sum over the whole series but subtract each
   element's own data set's means. Each data set is a balanced scan × point
   grid, so its two-way within transform is an exact orthogonal projection;
   the reduced problem is therefore the exact profile likelihood for the
   concentrations with every data set's TI and RI vector eliminated, not an
   approximation.
4. `small_a` is symmetric, so only one triangle is computed — halving the cost
   of the step that now does *n*<sub>datasets</sub>× the work of the old one.
5. Two latent problems in the same block are fixed while it is being restated:
   columns of *A* are indexed by their real stride (`narows`, which exceeds
   `ntotal` when Tikhonov regularization pads each column, so noise fitting and
   regularization were mutually exclusive), and the `BSave` path adds each data
   set's own noise back rather than the first set's.

### What per-data-set noise does *not* fix

`test/utils/test_us_solve_sim_mds.cpp` measures the recovered noise against
what was injected. TI alone and RI alone come back to better than 10⁻³ OD in
every data set. Both together do not: the recovered TI shape is off by around
5 × 10⁻³ OD.

That floor is not a multi-data-set effect, and the test asserts as much by
measuring it on a **single** data set first, where `US_SolveSimMDS` is
bit-identical to `US_SolveSim`. Its cause is the formulation both classes
inherit: the noise-reduced problem is handed to NNLS as its normal equations
*AᵀPA x = AᵀPb*, which squares the conditioning of an already ill-conditioned
Lamm basis. A little concentration leaks onto neighbouring grid points and the
difference surfaces in the TI vector. Fitting RI as well as TI adds a
scan-wise degree of freedom that widens the near-null space and makes the leak
larger.

That is a separate piece of work — feeding NNLS the projected design matrix
instead of its normal equations — and it belongs with `US_SolveSim`, not with
a copy made for a different reason. It bounds how exactly a 3DSA run can
report a TI vector; it does not affect the fitted distribution, whose residual
stays at the injected noise level in every data set.

### Status

Fixed for 3DSA. Cases 15–19 of the harness, which carry an `rmsd_max` the fit
should reach if noise fitting works and an `rmsd_spread_max` that catches the
"cleaned up one data set, left the rest" signature, now pass. `US_SolveSim`
itself is unchanged, so **global 2DSA through the MPI path still has this
defect**; the test
`TestSolveSimMDS.US_SolveSimLeavesLaterDataSetsWithoutNoise` asserts the old
behaviour deliberately, so that if `US_SolveSim` is ever fixed too, it fails
and says that `US_SolveSimMDS` can be retired.

---

## 4. Design decisions

| # | Decision | Rationale |
|---|---|---|
| **D1** | 3DSA is a **global, multi-dataset** analysis. Single-dataset operation is refused (not merely warned). | §3.3 — the fit is meaningless otherwise. Shipping a mode that silently returns a grid artefact would be worse than shipping nothing. |
| **D2** | Primary grid is (*s*<sub>20,W</sub>, *f/f₀*, v̄<sub>20</sub>). (*s*, *D*, v̄) offered as an alternate axis mapping. | *f/f₀* has a bounded, physically meaningful range (1–4) that is independent of *s*. A rectangular *s*×*D* grid spends most of its points on *f/f₀* < 1 — physically impossible shapes. *D* is reported as a derived output, so "fitting *s*, *D* and v̄" is satisfied either way. `US_Model::calc_coefficients()` supports the "s and D" input branch directly (`utils/us_model.cpp:224`), so the alternate mapping is nearly free — add an *f/f₀* ≥ 1 filter to drop non-physical points. |
| **D3** | New program `programs/us_3dsa/`, not a mode of `us_2dsa`. | (a) The GUI needs a multi-dataset series manager that `US_AnalysisBase2` does not provide; (b) 2DSA is GMP-validated and heavily used — a third axis in its control panel is a regression risk; (c) the MPI dispatch keys off `analysis_type`. Matches the existing `us_pcsa` precedent. |
| **D4** | Reuse `US_SolveSim::calc_residuals()` as-is for the inner solve; share grid generation and the identifiability metric via `utils/`. | The `stype > 9` path already does exactly the right thing per dataset. Duplicating a 1,800-line solver would be the single largest source of divergence bugs. |
| **D5** | Add per-dataset amplitude scale factors, fitted in an alternating outer loop. | §3.6. Small, well-defined, and removes a bias that would otherwise corrupt the headline number. |
| **D6** | Buoyancy-contrast gate in the GUI *and* in the MPI parser. | Cluster jobs bypass the GUI; the gate has to live where the fit starts, in both paths. |
| ~~**D7**~~ | ~~Column cache keyed on quantised (*s\**, *D\**), per dataset.~~ **Withdrawn in Phase 2** — measured saving is 1.00× at usable tolerances (§3B). The grid's image is 2-D but its points do not coincide; the v̄ axis slides them along the manifold. The result is near-collinearity, not redundancy. |
| **D8** | `US_Model::THREEDSA` appended at the **end** of `AnalysisType`. | The enum is serialised as a bare integer in model XML (`utils/us_model.cpp:707-712`). Inserting in the middle would silently reinterpret every stored model. |

---

## 5. Component inventory

### 5.1 `utils/` — shared by GUI and MPI

| File | Change | Detail |
|---|---|---|
| `us_solute.h/.cpp` | **new API** [done] | `init_solutes_3d( xlo, xhi, nx, ylo, yhi, ny, zlo, zhi, nz, grid_reps, s_mask, out )` producing `grid_reps³` interleaved subgrids. Writes values into the `US_Solute` field selected by the mask, matching `US_SolveSim::set_comp_attr()` conventions (`ATTR_S`→`.s`, `ATTR_K`→`.k`, `ATTR_V`→`.v`, `ATTR_W`/`ATTR_D`/`ATTR_F`→`.d`). Points are indexed rather than accumulated, so counts are exact; returns the effective `grid_reps`, clamped so no subgrid is empty. |
| `us_solute.h/.cpp` | **new API** [done] | `validate_mask( s_mask, QString& err )` — rejects any mask that puts two axes in the shared `.d` slot (e.g. *D* and *MW* together), duplicate attributes, and the three-bit values that name no attribute. |
| `us_solute.h/.cpp` | **new API** [done] | `physical_sdv( solute )` and `FF0_SPHERE_TOLER` — the *f/f₀* ≥ 1 filter for the rectangular *s*×*D* grid (§6.1). The tolerance exists because round-tripping an exact sphere through `calc_coefficients()` lands a few ulp below 1.0. |
| `us_solute.h` | **new enum** [done] | `US_Solute::attr_type`, static-asserted against `US_ZSolute`'s numbering so the two cannot drift apart. |
| `us_solve_sim_mds.h/.cpp` | **new** | `US_SolveSimMDS`: `US_SolveSim` copied, with per-data-set TI and RI noise (§3C). Shares `DataSet` and `Simulation` by typedef so either class takes the same inputs. `US_SolveSim` stays untouched, so 2DSA, PCSA, GA and DMGA are unaffected. |
| `us_solve_sim.h/.cpp` | **bug fix** [done] | The "vbar is constant" fast path was selected by `if ( attr_z != 3 )` — on the *position* of v̄ in the mask. With three genuinely varying axes that is wrong whenever v̄ lands in the z slot: dataset 0's cached corrections would be applied to every solute. Now `( attr_z != ATTR_V ) or fit_vbar`, identical to the old test whenever `fit_vbar` is false. |
| `us_solve_sim.cpp` | **bug fix** [done] | The same fast path took `vbartb`, `s20w_correction` and `D20w_correction` from `data_sets[0]` for *every* dataset in the loop. In a global fit each dataset has its own buffer and its own corrections; it now reads them from `dset`. See the note below. |
| `us_solve_sim.h` | **new field** [done] | `DataSet::fit_vbar` (bool, defaulted false). Set by the caller; drives the fix above. Also lets the existing 2DSA "vary vbar" mode state its intent instead of relying on attribute order. |
| `us_solve_sim_mds.h/.cpp` | **new** | `US_SolveSimMDS`: `US_SolveSim` copied, with per-data-set TI and RI noise (§3C). Shares `DataSet` and `Simulation` by typedef so either class takes the same inputs. `US_SolveSim` stays untouched, so 2DSA, PCSA, GA and DMGA are unaffected. |
| `us_solve_sim.h/.cpp` | **new static** [done] | `buoyancy_contrast( QList<DataSet*>&, double vbar_mid, QString& msg )` returning the largest pairwise gain over the series, plus `vbar_resolution()` and the `VBAR_CONTRAST_REFUSE` / `VBAR_CONTRAST_WARN` thresholds. Evaluates at `SolutionData::density_tb`, per §3A. |
| `us_model.h/.cpp` | **new enum value** [done] | `THREEDSA` appended to `AnalysisType`; `"3DSA"` added to the `typeText()` map. |
| `us_solve_sim_mds.h/.cpp` | **new** | `US_SolveSimMDS`: `US_SolveSim` copied, with per-data-set TI and RI noise (§3C). Shares `DataSet` and `Simulation` by typedef so either class takes the same inputs. `US_SolveSim` stays untouched, so 2DSA, PCSA, GA and DMGA are unaffected. |
| `us_solve_sim.h/.cpp` | *(deferred to Phase 2)* | `Simulation::scales`. A field nothing reads is a trap — a caller sets it and is silently ignored — so it lands with the outer loop that consumes it (§6.4). |
| `us_solve_sim.cpp` | *(not needed)* | `check_grid_size()` was listed for a 3-D extension, but it bounds Lamm-equation *time steps* from `s_max` alone. That is already dimension-independent. |
| `us_zsolute.h/.cpp` | *(no change)* | Already generalised; kept as the PCSA path. |

**Note on the second fix.** For a single-dataset fit `dset == data_sets[0]`, so results are
bit-identical; the change is visible only in a global fit through the mask path, which is precisely
where the old code was wrong. Callers that do not set `fit_vbar` keep the historical branch
selection exactly — `ATTR_V` is 3, and `us_solve_sim.cpp` static-asserts it so the equivalence
cannot silently break.

### 5.2 `programs/us_3dsa/` — new desktop program

| File | Derived from | Notes |
|---|---|---|
| `us_3dsa.h/.cpp` | `us_2dsa` | Main window. The significant new piece is a **dataset-series manager**: load *N* edited triples, resolve each one's solution/buffer record (ρ, η, *T*, D₂O %), and display the series with its computed buoyancy contrast. Recommended v1: keep `US_AnalysisBase2` for single-triple browsing and add an "Add to series" action, rather than rewriting the base class. |
| `us_3dsa_process.h/.cpp` | `us_2dsa_process` | `start_fit()` gains `vlo, vup, nvs`; `nsubgrid = ngrefine³`; `maxtsols = nsubp_s · nsubp_k · nsubp_v`; `estimate_steps()` recalibrated for cubic growth. **The depth-refinement scheduler transfers unchanged** (§2.5). `process_final()` collapses to a single mask-driven model-construction branch. |
| `us_worker_3d.h/.cpp` | `us_worker_2d` | `WorkPacket3D` = `WorkPacket2D` + `ll_v`, + `scales`. |
| `us_analysis_control_3d.h/.cpp` | `us_analysis_control_2d` | Adds v̄ lower/upper/steps counters; a second-axis selector (*f/f₀* | *D*); the dataset-series table; a live **v̄-resolution readout** from `buoyancy_contrast()`; the §7 memory estimator updated to cubic; and the D6 gate on Start. |
| `us_resplot_3d.h/.cpp` | `us_resplot_2d` | Residuals tabbed per dataset. |
| `us_plot_control_3d.h/.cpp` | `us_plot_control_2d` | `US_Plot3D` already offers any-two-of-{mw, s, D, f, f/f₀, v̄} with concentration as *z*; add v̄-slice and v̄-marginal views. |
| `CMakeLists.txt`, `us_3dsa.pro` | `us_2dsa` equivalents | `programs/CMakeLists.txt` globs subdirectories, so no registration edit is needed for CMake. |

Launcher registration: `P_3DSA` in `programs/us/us_win_data.h` (append before `P_END`),
an entry in `programs/us/us_win_data.cpp`, and an `addMenu( P_3DSA, … , velocity )` line in
`programs/us/us.cpp:210`.

### 5.3 `programs/us_mpi_analysis/` — cluster path

| File | Change |
|---|---|
| `3dsa_master.cpp`, `3dsa_worker.cpp` | New, from the 2DSA pair. The job/queue protocol is dimension-agnostic; real changes are confined to `init_solutes()` and subgrid counting. `US_Solute` already ships all five doubles over MPI. |
| `us_mpi_analysis.cpp` | Dispatch on `analysis_type.startsWith( "3DSA" )` (alongside the existing `"2DSA"` branch at `:1078`). |
| `us_mpi_parse.cpp` | Parse `vbar_min`, `vbar_max`, `vbar_resolution` / `vbar_grid_points`, and the axis mask; set `DataSet::fit_vbar`; apply the D6 gate and abort with a clear message when contrast is insufficient. |
| `us_mpi_analysis.cpp` (`write_model`) | A `THREEDSA` branch building components through the axis mask, and per-dataset model output carrying the fitted scale factors. |

`programs/us_3dsa_cli/` is the headless driver: `fit` runs a series described by a JSON case file
and reports the global and per-dataset RMSD, `gen-cases` writes the harness tree (§8.2). It links
only usutils and Qt Core, so it builds in the HPC profile as well as the application build, and it
is the entry point the harness and any future batch work use.

**External dependency:** the LIMS / job-submission layer that writes the job XML lives outside this
repository. It must learn the `3DSA` method and the new parameters, and must be able to submit a
*set* of edited triples as one job. This should be scheduled in parallel with Phase 4 — it is the
most likely schedule risk.

### 5.4 Downstream consumers to verify (no expected code change)

`us_fematch`, `us_pseudo3d_combine`, `us_modelmetrics`, `us_ddist_combine`, `us_combine_models`,
`us_query_rmsd`. Models emitted by 3DSA are ordinary models whose components happen to carry
varying `vbar20`; `us_pseudo3d_combine` already supports v̄ as a plot axis. Each needs a smoke test
against a 3DSA model, not new code.

`us_density_match` becomes the **independent cross-check**: its post-hoc v̄ estimate and 3DSA's
fitted v̄ should agree on the same data. This is the strongest validation available and is built
into the plan (§8.3).

---

## 6. Algorithms

### 6.1 Three-dimensional grid generation

Direct extension of `US_Solute::init_solutes()`. For axis lengths (*n<sub>x</sub>*, *n<sub>y</sub>*,
*n<sub>z</sub>*) and `grid_reps` = *g*, the grid step on each axis is the point spacing times *g*,
and *g*³ interleaved subgrids are emitted, subgrid (*i*, *j*, *k*) starting at offset
(*i*, *j*, *k*) point-spacings. Each subgrid is a coarse covering of the whole box; their union is
the full grid, with no point emitted twice. Retain the existing 1 % overscan and the "omit *s* ≈ 0"
guard.

With the D2 mapping the *y* axis is *f/f₀*; with the alternate (*s*, *D*, v̄) mapping, points whose
implied *f/f₀* < 1 are dropped at generation time.

### 6.2 Depth refinement

**Unchanged from 2DSA.** `process_job()` accumulates surviving solutes across completed subgrids
and re-queues their union at the next depth until a single final pass remains. Nothing in that
logic is dimensional. Only `nsubgrid`, `maxtsols` and the progress estimator need cubic-aware
values.

### 6.3 Column cache keyed on (*s\**, *D\**) — withdrawn on measurement

The plan proposed caching simulated columns keyed on quantised (*s\**, *D\**),
reasoning from §3.1 that the 3-D grid maps onto a 2-D manifold per dataset and so a large fraction
of grid points must request simulations indistinguishable from one already computed. Phase 2
measured it over a production 64 × 64 × 16 grid and the saving is **1.00× at 10⁻⁵ relative** and
1.20× at 10⁻³ — see §3B for the full table.

The inference was wrong, in an instructive way. The grid's *image* is two-dimensional, but its
points do not pile onto each other: moving along the v̄ axis slides a point **along** the manifold,
not onto a neighbour. Confinement to a surface produces near-*collinearity*, not *duplication* —
a conditioning problem, not a redundancy one. It is the same conditioning that exposed the NNLS
bug in §3B.

A cache only pays at 10⁻² relative, coarser than the data noise, where it would corrupt the fit.
Design decision D7 is withdrawn.

What the observation *does* suggest, and what is worth investigating instead, is a **coarse-then-
refine v̄ scan**: because the v̄ axis moves a point smoothly along the manifold, a coarse v̄ pass
locates the region and a fine pass need only cover it.

A natural companion: extend the existing `_NORM_CUTOFF_` column culling
(`utils/us_solve_sim.cpp:616`), which currently drops near-*zero* columns, to also drop
near-*collinear* ones via a cheap Gram test.

### 6.4 Per-dataset amplitude scale factors

Alternating outer loop around the existing NNLS:

1. Initialise α<sub>e</sub> = 1 for every dataset *e*.
2. Solve the NNLS for the concentrations **x** with rows of dataset *e* scaled by α<sub>e</sub>.
3. For each *e*, update in closed form
   α<sub>e</sub> ← ⟨**b**<sub>e</sub>, **ŝ**<sub>e</sub>⟩ / ⟨**ŝ**<sub>e</sub>, **ŝ**<sub>e</sub>⟩,
   where **ŝ**<sub>e</sub> is the simulated block for dataset *e*.
4. Fix the gauge (e.g. α₁ ≡ 1, or Σα<sub>e</sub> = *N*) so the scales and the concentrations are
   not jointly degenerate.
5. Iterate to convergence — expected in a handful of passes, since step 3 is exact given **x**.

Report the fitted α<sub>e</sub> in the run report: a value far from the ratio of nominal loading
concentrations is a strong signal of a data problem.

*Phase-1 fallback:* pre-normalise each dataset by its total loading signal. Simpler, no solver
change, but the normalisation error propagates into v̄ — acceptable for the first validation runs
only, and it should not ship as the default.

### 6.5 Identifiability metric (D6 gate)

For the loaded series and the midpoint of the requested v̄ range, compute for every pair (*e₁*, *e₂*) — with ρ taken from `SolutionData::density_tb`, not the nominal buffer density (§3A) —

$$G_{e_1e_2}=\left|\frac{\rho_{e_1}}{1-\bar v\rho_{e_1}}-\frac{\rho_{e_2}}{1-\bar v\rho_{e_2}}\right|$$

and take *G*<sub>max</sub> over pairs. Report the implied resolution
δv̄ ≈ (relative *s*-precision) / *G*<sub>max</sub>, using an *s*-precision estimated from the data
RMSD. Gate: refuse below ≈ 0.5 (mL/g)⁻¹, warn below ≈ 1.0, and always display the number next to
the v̄ grid controls so the user sees the consequence of their v̄ step size before starting.

---

## 7. Cost and memory

### 7.1 The scaling problem

2DSA default: 64 × 64 = 4,096 columns. The naive 3DSA analogue at 64 × 64 × 64 would be 262,144
columns × *N* datasets — a 64 *N*-fold increase in Lamm-solver calls. That is not viable.

### 7.2 Why it is nevertheless tractable

Three independent factors, none of them speculative:

1. **The v̄ axis is intrinsically coarse (§3.4).** Resolution is 0.005–0.01 mL/g; a 0.60–0.85 range
   needs 16–25 points. Take *n<sub>v</sub>* = 16 as the default → 65,536 columns, a 16× increase,
   not 64×.
2. ~~**The column cache (§6.3)** removes 3–10× of that.~~ **Withdrawn** — measured at 1.00×
   (§3B, §6.3). The columns are near-collinear, not duplicated.
3. **Subgridding already bounds the working set.** At `grid_reps` = 8 the 3-D grid yields 512
   subgrids of 128 points — comparable to the 64 subgrids of 64 points that 2DSA handles today. Per
   task, the NNLS matrix stays the same order of magnitude; what grows is the *number* of tasks,
   which is the parallelisable dimension.

**Net expectation, corrected after Phase 2.** With the cache withdrawn, the third axis costs what it
appears to cost: *n<sub>v</sub>* × a 2DSA run per dataset — **16× at the default** — times the
number of datasets, plus the amplitude iterations (7 measured, each a full pass).

Measured throughput on the Phase 2 test geometry (4 scans × 261 points, 200 simpoints) was ≈ 1,200
Lamm solves per second across four threads. A production geometry is one to two orders of magnitude
heavier per solve, which puts a full 64 × 64 × 16 three-dataset fit in the region of several hours
on four cores and tens of minutes on a cluster: **desktop-marginal, cluster-comfortable**. That is
now measurement-backed rather than estimated.

Two mitigations remain and both are real — the v̄ axis is intrinsically coarse (§3.4), and only
depth 0 is proportional to the grid size. A third, the coarse-then-refine v̄ scan of §6.3, is the
replacement for the cache and should be evaluated before the GUI settles its defaults.

### 7.3 Memory

The estimator in `us_analysis_control_2d.cpp:855-864` models per-thread memory as a linear function
of (subgrid points + noise vectors) × (scans × points). For 3DSA:

* `nsbpts` becomes `(nsteps/g) · (nstepk/g) · (nstepv/g)`;
* `ntconc` becomes the sum of scans × points **over all datasets in the series**.

Both must be threaded through `memory_check()`, and the dialog must state the dataset count that
produced the estimate. The existing `US_SolveSim::checkGridSize()` guard needs its 3-D limits.

---

## 8. Validation

### 8.1 Unit tests (`test/utils/`, auto-discovered by the existing gtest glob)

* `test_us_solute3d.cpp` — **delivered in Phase 1**, 14 checks: grid point counts; the subgrid
  partition covers the box exactly once and leaves no subgrid empty; endpoints are hit exactly;
  `grid_reps` = 1 degenerates to a single full grid and an over-large `grid_reps` is clamped; axis
  values land in the field the mask names; s ≈ 0 points are omitted; mask validation rejects
  (*D*, *MW*) pairs, duplicates and unknown attributes; the *f/f₀* ≥ 1 filter drops exactly the
  unphysical points of the (*s*, *D*, v̄) box and nothing from the (*s*, *f/f₀*, v̄) one.
* `test_us_solve_sim_vbar.cpp` — **delivered in Phase 0**; results in §3A. Encodes the physics as
  six executable checks:
  1. `ParameterisationsAreEquivalent` — (*s*, *f/f₀*, v̄) and (*s*, *D*, v̄) yield identical
     (*s\**, *D\**);
  2. `SingleDatasetFibreIsExact` — for a **single** dataset the fibre is reproduced to machine
     precision across a molar-mass range of 2.65×;
  3. `LammSolverIgnoresVbar` — two fibre points give bit-identical `US_Astfem_RSA` output;
  4. `DensityContrastSeparatesTheFibre` — a **second density** spreads the same fibre by > 100 %;
  5. `BuoyancyGainMatchesCorrections` — the closed form is the derivative of `data_correction()`
     when evaluated at `density_tb`, and the §3.4 table is reproduced;
  6. `ContrastGateClassifiesSeries` — the three example series fall on the intended sides of the
     refuse/warn thresholds, and a single dataset has identically zero gain.

  Checks (2) and (3) are the guard that stops anyone from "helpfully" re-enabling single-dataset
  3DSA later. Phase 1 added seven more to the same file, exercising the real `buoyancy_contrast()`,
  `vbar_resolution()`, the gate thresholds, the `fit_vbar` default, and the fact that the
  corrections are v̄-sensitive only under contrast — which is why the cached-versus-recomputed
  distinction matters at all.

* `test_us_solve_sim_mds.cpp` — **delivered with the noise fix** (§3C), 6 checks: on a single data
  set `US_SolveSimMDS` reproduces `US_SolveSim` bit-for-bit at every noise flag; on a two-data-set
  series it recovers each cell's own TI and RI vector to better than 10⁻³ OD, and each recovered
  block matches its own cell's profile rather than the other cell's; a no-noise series is
  unaffected. The sixth asserts the *old* behaviour of `US_SolveSim` — later blocks left at zero —
  so that if it is ever fixed too, the test fails and says `US_SolveSimMDS` can be retired. The
  combined TI+RI test measures the normal-equations floor on one data set first, so that a series
  failure cannot be blamed on the wrong thing.

### 8.2 Synthetic end-to-end — the harness

`test/3dsa_harness/` drives the full round trip: `us_3dsa_cli gen-cases` writes the case tree,
`us_astfem_sim` produces the data through its own CLI, and `us_3dsa_cli fit` fits each series and
reports the **global RMSD and the RMSD of every data set**. `test/3dsa_harness/README.md`
documents it; `run_harness.py --outdir DIR --bindir BINDIR` runs it.

Twenty-four cases, covering every axis that matters:

| group | cases | what they exercise |
|---|---|---|
| Series length | 1–3 | two, three and five isotope concentrations |
| Loading | 4–5 | loadings spread 1.0 / 0.7 / 1.4 and 1.0 / 0.4 / 2.0 |
| Mixtures | 6–12 | species differing in *s*, in *D*, in v̄ and in combination; two and three components; mixtures sharing a v̄ while differing in *s* and *D* |
| Noise | 13–19 | random, time-invariant and radially-invariant noise, alone and together, with the fit asked to solve for TI and RI |
| Gates and edges | 20–24 | weak contrast and a single dataset, both of which **must be refused**; v̄ at both grid edges; a 1.5 S / 8.0 S mixture |

Every species is declared in standard (20W) space. `us_astfem_sim` converts to experimental space
per component using that component's own v̄ and the buffer it is given, and the fit has to invert
that. The round trip is the point — a harness that pre-converted would only be testing the NNLS.

Acceptance per case: recovered concentration-weighted v̄ and *s* within the case's tolerance,
fitted amplitude factors matching the imposed loading ratios, and the two negative controls
refused.

Two caveats stand, both recorded in the harness README: D₂O density and viscosity are interpolated
linearly (harmless, since the same numbers reach both the simulator and the fit), and H/D exchange
is not modelled, so the recovered truth is cleaner than a real experiment's (§3.5).

### 8.3 Cross-check against `us_density_match`

On real density-series data, 3DSA's fitted v̄ and `us_density_match`'s post-hoc v̄ must agree within
their combined uncertainty. Disagreement is informative either way and should be understood before
release.

### 8.4 Regression

2DSA and PCSA results must be bit-identical before and after the `us_solve_sim.cpp:723` fix and the
`Simulation::scales` addition — both are gated on new fields that default to the current behaviour.
Run the existing 2DSA test corpus and diff the output models.

---

## 9. Phased delivery

| Phase | Content | Exit criterion | Est. |
|---|---|---|---|
| **0 — Spike** ✅ | `test_us_solve_sim_vbar.cpp` only: demonstrate the single-dataset degeneracy and two-density identifiability numerically. | **Done** — see §3A. Degeneracy exact to machine precision, confirmed through `US_Astfem_RSA`; §3.4 table reproduced; one correction found (`density_tb`). | 2 d |
| **1 — utils** ✅ | 3-D grid generation, mask validation, `buoyancy_contrast()`, `THREEDSA`, the `attr_z != 3` fix, `fit_vbar`. | **Done** — 27 tests green across `test_us_solute3d.cpp` and `test_us_solve_sim_vbar.cpp`; the full HPC target including `us_mpi_analysis` builds clean; both solver fixes are no-ops for existing single-dataset callers. A second latent bug found and fixed — see §5.1. | 3 d |
| **2 — Engine** ✅ | `US_3dsaProcess`, the parallel level runner, the amplitude loop. Driven headless from a test harness. | **Done** — see §3B. v̄ recovered to 1×10⁻⁴ mL/g and the loadings to four decimals on a synthetic three-buffer series; 7 engine tests green. The column cache was measured and withdrawn; two bugs outside 3DSA found and fixed. | 2 wk |
| **2a — Noise** ✅ | `US_SolveSimMDS`: the solver copied and its TI/RI noise algebra made data-set aware, so every run of a series carries its own noise. | **Done** — see §3C. All 24 harness cases pass, including the five that fit noise; the worst per-data-set RMSD on the TI case fell 45×, and on the RI case 60×. 6 new tests; `US_SolveSim` untouched. | 3 d |
| **3 — GUI** | `us_3dsa` main window + dataset-series manager, control panel, gate, plots, launcher registration. | An analyst can run the §8.2 case end to end from the menu. | 3 wk |
| **4 — MPI** | `3dsa_master`/`3dsa_worker`, parser, model writer, gate in the parser. LIMS coordination in parallel. | Cluster job completes and returns models equivalent to the desktop run. | 2 wk |
| **5 — Docs & integration** | `doc/manual/source/3dsa/`, help pages wired to `showHelp`, downstream consumer smoke tests, §8.3 cross-check. | Manual builds; consumers verified; cross-check documented. | 1 wk |

Phases 0–2 are the ones that can invalidate the concept, and they are deliberately front-loaded.
Phases 0, 1, 2 and 2a have now run: §3.4 reproduced (§3A), the shared `utils/` layer in place, the
engine recovering v̄ from a synthetic series (§3B), and every data set of a series carrying its own
systematic noise (§3C). The design stands, with D7 withdrawn on measurement. All 24 harness cases
pass. Phase 3 can begin.

### Future work (explicitly out of scope here)

* **Per-dataset H/D exchange correction** for D₂O series (§3.5). The data-model hook is in Phase 1;
  the correction itself is a separate piece of physics work.
* **Radial density gradient ρ(*r*)** — self-generating gradients (CsCl, Nycodenz) make v̄
  identifiable *within a single run*, which would remove the multi-dataset requirement entirely.
  `US_Model::coSedSolute` already exists as a placeholder, but the ASTFEM engine has no ρ(*r*)
  support, so this is a substantial solver project.
* Tikhonov regularisation tuned for the 3-D grid; Monte Carlo error bars on v̄.

---

## 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The degeneracy is not accepted and single-dataset 3DSA is requested anyway | Ships a number that looks like a measurement and is not | §8.1 test (2) as an executable guard; the gate refuses rather than warns; the manual states it plainly |
| LIMS-side work not scheduled | Cluster path unusable; desktop-only release | Raise at Phase 0; Phases 0–3 deliver a usable desktop program independently |
| Buffer ρ/η metadata inaccurate in real datasets | v̄ biased with the §3.4 gain | Series panel shows provenance of each value; cross-check §8.3 |
| Wall-clock worse than the §7.2 estimate | Desktop path impractical | Phase 2 measures it headless, before GUI investment; column-cache tolerance is tunable |
| Copy-and-adapt duplication (`us_2dsa` → `us_3dsa`) drifts over time | Maintenance burden | Shared code pushed into `utils/` wherever it is genuinely shared; the duplication is confined to the scheduler and GUI, matching the existing `us_pcsa` precedent |

---

## 11. Summary of the answer to the original question

*Can we fit s, D and v̄ simultaneously, the way 2DSA fits s and D?*

**Not from one experiment** — the Lamm equation cannot see v̄, so the third axis is unidentifiable
and any value returned would be a grid artefact.

**Yes from a buoyancy-contrast series** — and the existing UltraScan solver is already most of the
way there: it stacks datasets into one NNLS problem and already has a code path that varies v̄ per
solute with per-dataset corrections. The work is a 3-D grid generator, a scheduler with cubic
subgrid counts, per-dataset amplitude scaling, a column cache to keep the cost down, a GUI that
manages the dataset series and refuses under-determined fits, and the MPI plumbing to match.
