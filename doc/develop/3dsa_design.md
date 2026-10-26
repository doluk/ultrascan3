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

The review branch merged in `552482e` reached the same fix independently and
corrected two more defects in the same loop: after a round-off removal the
following coefficient was skipped — neither shifted nor rotated — and at the
iteration limit the routine accepted unchecked values and returned a residual
norm that belonged to a different *x*. It now stops with the last feasible *x*
and that *x*'s residual. Its version of `us_math2.cpp` was taken whole;
`test_us_math2.cpp` carries its tests for the tie case, the Kuhn-Tucker
conditions and the iteration limit.

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

`test/3dsa_harness/` (§8.2) runs synthetic series through `us_astfem_sim` and
`us_3dsa_cli`. On its first run, with 24 cases, nineteen behaved as designed.
The five that fit noise exposed a defect that was not in 3DSA at all. It is now
fixed in `US_SolveSim` itself, for every caller, and every case passes. This
section records what was wrong, how it was fixed, and a claim made here earlier
that turned out to be wrong.

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

Three problems. The harness found the first; a review of `US_SolveSim`'s noise
path that followed it (branch `claude/nnls-us-solve-sim-review-eufblk`, merged
in `552482e`) found the other two.

1. **Only the first data set was centred.** All seven helpers behind the noise
   algebra in `US_SolveSim::calc_residuals` read `data_sets[ d_offs ]` — the
   first data set — and looped over *that* set's `pointCount()` and
   `scanCount()`: `compute_a_tilde`, `compute_L_tildes`, `compute_L_tilde`,
   `compute_a_bar`, `compute_L_bars`, `ti_small_a_and_b`, `ri_small_a_and_b`.
   The surrounding code sized the noise vectors for the whole series and the
   residual loop indexed them per data set, so the vectors had room for the
   series, but only the first block was ever written; the rest stayed zero and
   no noise was subtracted from those data sets.
2. **With TI and RI together, RI was not removed while the concentrations were
   fitted.** The reduced system subtracted from each element the per-radius
   mean of scan-demeaned values (`a_bar`, `L_bars`), but never the per-scan
   mean itself. That is not the two-way within transform: the scan-wise noise
   stayed in the problem the concentrations were fitted to, and RI was only
   computed afterwards, from whatever concentrations came out.
3. **NNLS ran on the normal equations.** The reduced problem was handed to NNLS
   as *AᵀPA x = AᵀPb*. Once non-negativity constraints are active, the NNLS
   solution of the normal equations does not minimise the residual of the
   original problem, and forming *AᵀA* squares the conditioning of an already
   ill-conditioned Lamm basis.

The same code also read columns of *A* with stride `ntotal`, although Tikhonov
rows make them `ntotal + nsolutes` long, so noise fitting and regularisation
could not be combined.

### Scope

None of this was a 3DSA regression. It predates this work and affected every
fit that asks for TI or RI noise: problem 1 any multi-data-set fit, including
**global 2DSA through the MPI path**; problems 2 and 3 every fit, single data
set included. A single-data-set fit with TI noise alone came close enough to
right that the defects survived.

### The fix

**In `US_SolveSim`, for every caller.** The review branch replaced the seven
helpers with one static, `nnls_noise()`. It centres each data set's rows of *A*
and *b* by that set's own per-scan means (RI), per-radius means (TI), or both,
leaves Tikhonov rows alone, and solves NNLS on the projected *design matrix*
rather than on its normal equations. For a balanced scan × point grid — which
each data set is — the two-way centring is an exact orthogonal projection onto
the complement of that set's noise subspace, so the concentrations are the true
least-squares solution with every data set's TI and RI eliminated. The noise
vectors follow from the removed means, one block per data set in turn.

**An interim copy, now retired.** Before that fix existed, 3DSA ran on
`US_SolveSimMDS` (`utils/us_solve_sim_mds.{h,cpp}`, added in `599d219`): a copy
of `US_SolveSim` whose helpers looped over every data set rather than the first,
so that 2DSA, PCSA, GA and DMGA would not change underneath their users. It
fixed problem 1 and the Tikhonov stride, but not problems 2 and 3. With
`US_SolveSim` fixed for everyone the copy had nothing left to offer and fell
short of the fixed solver on both of those. `US_3dsaProcess::run_task()`
constructs `US_SolveSim` again; the copy and its test are gone.

### A correction

The copy's header, and an earlier version of this section, described its
reduced system as the two-way within transform and "the exact profile
likelihood for the concentrations with every data set's TI and RI vector
eliminated". That was wrong. The copy restated the original algebra data set by
data set and so inherited problem 2: with TI and RI fitted together it was
neither the two-way transform nor exact.

The same section then attributed the copy's remaining error — a recovered TI
shape off by about 5 × 10⁻³ OD when TI and RI were fitted together — to the
normal equations alone, and said it "does not affect the fitted distribution".
Problem 2 contributed too — the fix addressed both together, so the
measurements below cannot apportion the error between them — and the
distribution was affected: concentration leaked onto neighbouring grid points.
Measured on the copy's own test setup — a grid containing the true solute,
injected TI and RI, five scans — before and after:

| | TI shape error | residual RMSD | concentration on the true solute |
|---|---|---|---|
| copy, one data set | 4.2 × 10⁻³ | 3.7 × 10⁻⁴ | 0.4990 of 0.5 |
| copy, two data sets | 1.0 × 10⁻² / 4.6 × 10⁻³ | 8.8 × 10⁻⁴ / 4.9 × 10⁻⁴ | 0.4971 of 0.5 |
| `US_SolveSim`, one data set | < 10⁻⁶ | 4 × 10⁻¹⁶ | 0.500000 |
| `US_SolveSim`, two data sets | < 10⁻⁶ | 4 × 10⁻¹⁶ / 5 × 10⁻¹⁶ | 0.500000 |

There is no floor left: noise-free data with injected TI and RI is now fitted to
machine precision.

### Status

Fixed for every caller. `test/utils/test_us_solve_sim.cpp`, from the review
branch, covers it: `EachDataSetHasItsOwnNoise` exercises `nnls_noise` on two
data sets of different sizes, and `GlobalFitRemovesNoiseOfEachDataSet` runs
`calc_residuals` on such a pair and recovers both sets' concentrations and noise
to 10⁻⁹. Both fail if only the first data set is centred.

On the harness the two solvers were compared on identical simulated data
before the copy was removed: identical wherever no noise is fitted, and an equal
or lower residual from `US_SolveSim` on every case that fits it (15–19 and 28),
with all 31 cases passing either way.

## 3D. What threading found — the headless solver keeps process-wide state

Investigating a `main` test that fails in this container
(`TestUSSolveSimCache.FitIsUnchangedByCacheInThreads`) turned up two defects in
the Lamm-equation solver, both confined to `NO_DB` builds — the HPC profile that
`us_mpi_analysis` and `us_3dsa_cli` are built in. Both assume that a process only
ever runs one ASTFEM solve at a time. They were confirmed with ThreadSanitizer,
with a patch that removes each one, and with standalone reproductions; the
upstream report is in hand.

1. **Function-static work arrays.** `US_Astfem_RSA::calculate_ni`, its three
   `ComputeCoefMatrix*` helpers and `US_AstfemMath::tridiag` keep their scratch
   storage in function-local statics, sized once and reused by every later call.
   Each solve fills them with coefficients for its own solute, so two threads
   overwrite each other's matrices, and a larger grid frees them under a running
   solve. The desktop build allocates per call and is unaffected.
2. **A temporary timestate named by process and second.** A data set without a
   loaded timestate makes `US_Astfem_RSA::calculate()` write
   `../p<pid>t<yyMMddhhmmss>.time_state.tmst` — only if that file does not exist
   yet — and read it back. Concurrent threads read each other's half-written
   file; and, **single-threaded**, a second data set with a different speed
   profile simulated within the same second gets the first one's.

### What it did to 3DSA

| Situation, HPC build | Before | After |
|---|---|---|
| Eight subgrids, four threads requested | 2 of 3 runs segfault; the third returns RMSD 2.16 × 10⁻³ against 3.26 × 10⁻⁴ serial | runs serially, says why, bit-identical to the serial fit (3 of 3) |
| Series whose cells ran at different speeds and run times (harness case 33), **one thread** | v̄ **0.7923** against a truth of 0.7300, residual 3.49 × 10⁻² — every run, identically | v̄ 0.7295, residual 1.86 × 10⁻⁴ |

The second row is the serious one: a real buoyancy-contrast series is several
runs, rarely at one speed, and the old engine returned a confidently wrong v̄ for
it with nothing to show anything was amiss.

The harness never saw either. Its cases fit one subgrid (`grid_reps` = 1), so
every level is a single task and the engine runs it on one thread whatever
`--threads` says; and every series ran all its cells at one speed for one time,
so a shared speed profile happened to be the right one.

### What 3DSA does now

Neither defect can be fixed from outside `us_astfem_rsa.cpp` and
`us_astfem_math.cpp`, so the engine avoids both, without touching the shared
solver:

- **Every data set gets its own timestate before any solve**
  (`US_3dsaProcess::prepare_timestates()`): its run's own file when
  `DataSet::tmst_file` names one, otherwise one written exactly as `calculate()`
  would write it — same grid, same scans, same `sim` flag — under a name of its own
  in a private temporary directory. This runs in the calling thread on the
  engine's own copies of the data sets; the callers' data sets are not touched.
  No solve reaches the temporary-file path any more. The synthesized file
  reproduces `calculate()`'s own exactly: a fit through it is bit-identical to
  the old single-threaded fit, and on case 33 the run's own files and the
  synthesized ones give the same fit to the last digit.
- **Solves run serially where they cannot run concurrently**
  (`US_3dsaProcess::concurrent_solves_safe()`, false in `NO_DB` builds).
  `fit()` caps the worker count at one there, emits a message saying why, and
  reports the count actually used in `Result::nthreads`; `us_3dsa_cli` prints
  "threads requested 4, used 1" and records both in its JSON result. Desktop
  builds keep their threads.
- `us_3dsa_cli` points each data set at `<run_dir>/<run_id>.time_state.tmst`
  when the simulator wrote one, as a real analysis would use the run's own.

The cost is speed in the headless CLI only: with the work arrays made per-call
in a scratch build, four threads ran case 9 at eight subgrids 3.2× faster
(4.5 s against 14.5 s) with a bit-identical result. That is what lifting the cap
will buy once `main` allocates per call (or per thread) in `NO_DB` builds;
`concurrent_solves_safe()` is the one place to change. The MPI path (Phase 4) is
one solve per process and unaffected by the first defect; its workers will still
need a timestate per data set, and `prepare_timestates()` is there for that.

### Not verified here

The desktop build's threaded path cannot be built in this container (no Qwt for
Qt 6). `TestUS3dsaProcess.ThreadsDoNotChangeTheFit` asserts that four threads and
one give a bit-identical fit, and will exercise exactly that path in CI, where the
unit tests are built with the database.

---

## 3E. Density gradients — one run can determine v̄

§3.3's degeneracy rests on one assumption: every solute in a run sees the same density. A buffer
that forms a gradient during the run breaks it. Co-sedimenting components (CsCl, Nycodenz) sediment
into a density gradient; a band-forming cell whose lamella and cell solution differ in a
co-diffusing component (sucrose) relaxes into one. A solute's local sedimentation coefficient is
then

$$s(r,t)=s_{20,W}\,\frac{\eta_{20,W}}{\eta(r,t)}\,\frac{1-\bar v\,\rho(r,t)}{1-\bar v\,\rho_{20,W}}$$

and it moves through a range of densities within one cell. ABDE and DGE run that to its limit: a
solute bands where ρ = 1/v̄. This section brings those experiments into 3DSA.

### The solver: ASTFVM from the `cosedimenting` branch

ASTFEM has no ρ(*r*, *t*) (§9, future work). The finite-volume solver on the `cosedimenting`
branch has: `US_LammAstfvm` adjusts *s* and *D* at every node and time step from a density and
viscosity built as the buffer's own values plus each gradient's increment. Two gradient sources:

* **co-sedimenting** — `US_LammAstfvm::CosedData`, the gradient-forming components themselves
  simulated (with ASTFEM) on the experiment's grid, and interpolated in *r* and *t*;
* **co-diffusing** — `US_Math_BF::Band_Forming_Gradient`, the analytic (Bessel-series) relaxation
  of a lamella's excess component, tabulated over the run.

The solver, `US_Math_BF`, `US_Buffer`'s co-sedimenting components, `US_SimulationParameters`' band
forming fields and the matching `US_Astfem_RSA` overload were taken from that branch as they are
(`4751867`). ASTFEM fits are bit-identical before and after: the unit suite and seven harness
refits were compared.

### What 3DSA does with it

| Piece | What it does |
|---|---|
| `US_FvmGradients` (new, `utils/`) | Prepares a data set's gradients **once per fit**, as the cosedimenting 2DSA does: splits the components into lamella and cell solution, takes the base buffer, simulates each co-sedimenting component and builds the band-forming solution. The grid is the whole cell (meniscus to bottom, ≤ 0.002 cm) at 100 times from the end of acceleration to the last scan, so an edit that keeps one scan still gets the gradient's history. Records the lowest and highest density in the cell. Copy-constructed per worker thread, since both gradients keep working state while read. |
| `US_SolveSim` | A data set whose simparams select the ASTFVM is simulated by `US_LammAstfvm` with the prepared gradients, from the **standard-space** solute (the solver corrects locally; correcting it first would apply the buffer twice). The simulation-cache key includes the gradient-forming conditions. |
| `buoyancy_contrast()` | A gradient data set contributes its lowest and highest density, a plain one its `density_tb`. So a single gradient run passes the gate on its own, and a plain single run is still refused. |
| `US_3dsaProcess` | Prepares gradients after the timestates and before the gate. Refuses a gradient-forming buffer on an ASTFEM mesh (it would be ignored). `equilibrium_note()`: see below. |
| `us_3dsa_cli` | A data set may name an `edit` (real data: excludes, range, meniscus, bottom and linear baseline applied, the run's timestate found beside it), a `buffer` XML and `last_scan_only`. A `simulate` subcommand writes synthetic data with the same simulator, since `us_astfem_sim` on this branch cannot simulate a gradient buffer. `"equilibrium"`, `"verify"` and `"mask": "w_k_v"` select the closed form below, its check and a molar-mass axis; the fit lists the solutes found with their densities and shares. |
| `US_Buffer::writeToDisk` | **Bug fix.** Co-sedimenting coefficients were written with empty attribute names (`QString::asprintf` is static and its result was discarded) and five decimals, so a gradient buffer saved to disk read back with no gradient. |

### Equilibrium data: a closed form instead of the solver

An ABDE edit keeps the last scan. Two things follow.

**The bands are too narrow for a coarse v̄ grid.** A band is a few hundredths of a centimetre wide;
in a gradient of 0.33 g/mL per cm a v̄ step of 0.01 moves it by 0.016 cm, more than its width. On
the synthetic AAV case below, a 5 × 1 × 9 grid (v̄ step 0.01) solved with the ASTFVM took 883 s and
left RMSD 1.58 against noise of 0.003. The data determine v̄ to about 10⁻⁴, so the grid has to
step near 0.0005 — hundreds of v̄ values — and at 15 s per finite-volume solve that is hours.

**At equilibrium the profile has a closed form.** A solute that equilibrates faster than its
gradient changes sits at sedimentation equilibrium in the density of the moment:

$$c(r)\propto\exp\!\left(\frac{M\omega^2}{RT}\int_m^r\bigl(1-\bar v\,\rho(r',t)\bigr)\,r'\,dr'\right)$$

with its signal conserved over the sector. `US_SolveSim::equilibrium_profile()` computes it on a
fine whole-cell grid (≤ 0.0005 cm) from the prepared gradient's density at the scan's time
(`US_FvmGradients::density_at()`, cached), or from the buffer's density, and interpolates onto the
data. `Parameters::equilibrium` selects it for every data set (`DataSet::equilibrium`), and the
cache key says so. Against the ASTFVM on the AAV case it puts the bands at the same radius to the
0.002 cm grid, with peak heights within 0.05 % and the largest local difference about 1 % of the
peak — in well under a millisecond instead of 6–24 s per solute.

It is only as good as the equilibrium. After 2 h in 1.5 M CsCl (the unit tests' run) the solutes
are still travelling: the solver's peaks lie up to 0.11 cm from the closed form's and the profiles
differ by 83–88 % of the peak. So `us_3dsa_cli`'s `verify` refits the solutes that carry the signal
(down to 0.5 % each, at most 16) with the Lamm-equation solver and with the closed form, and
reports both RMSDs; the solver's well above the closed form's says the run was too short for it.

**What one equilibrium scan determines.** A band's position gives its density, 1/v̄, and its area
the signal. Its width is set by the molar mass *and* by any spread in density, and NNLS can trade
one against the other: on the AAV case every band went to the largest molar mass on the grid,
spread over v̄ values 0.001–0.002 apart. Shape (*f/f₀*) does not enter at all. So the fit reports
densities and shares; the molar mass needs the approach to equilibrium (the full scan set and the
ASTFVM) or an assumption. `equilibrium_note()` says this in the progress messages and the report
whenever every data set holds a single scan, or the fit simulates equilibrium, and two hydrodynamic
axes vary; `mask_w_k_v()` puts molar mass on the first axis.

### Measured

| Case | Data | Grid | Result | Cost |
|---|---|---|---|---|
| CsCl run, unit test `RecoversVbarFromOneGradientRun` | 1.5 M CsCl, 48 krpm, 2 h, 6 scans; 10 S, *f/f₀* 1.5, v̄ 0.80 | 5 × 3 × 5 | v̄ 0.8000, *s* 10.000 S, RMSD 1.6 × 10⁻⁵; the same cell in water is refused | 75 solves, 11.8 s |
| Band forming, `RecoversVbarFromABandFormingRun` | 15 µL lamella with 0.3 M sucrose onto 1.5 M CsCl; same solute | 3 × 1 × 5 | v̄ 0.8000, *s* 10.000 S, RMSD 1.0 × 10⁻⁶ | 15 solves, 18.3 s, plus ≈ 12 s for the band-forming table |
| ABDE-like, ASTFVM per point | AAV empty (64 S, v̄ 0.763, 0.3 OD) and full (95 S, v̄ 0.709, 0.5 OD) in 2.8 M CsCl, 48 krpm, 9.7 h, 3 × 10⁻³ OD noise; **last scan only** | 5 × 1 × 9, v̄ step 0.01 | RMSD 1.58 — the v̄ grid is far too coarse for the bands | 45 solves, 883 s |
| ABDE-like, closed form (`"equilibrium": true`) | the same | 5 × 1 × 161, v̄ step 0.0005 | RMSD 2.94 × 10⁻³ (noise 3 × 10⁻³). Empty: v̄ 0.76305, density 1.3105 (1.3106), signal 0.3001. Full: v̄ 0.70897, density 1.4105 (1.4104), signal 0.5001. Empty fraction 37.5 % (37.5 %). Refitted with the ASTFVM (all 30 solutes, before `verify` took a subset): RMSD 2.9404 × 10⁻³ | 805 profiles, 0.6 s; verify ≈ 16 min |
| Closed form, `EquilibriumFitFindsEachBandsDensity` | two bands (v̄ 0.83 and 0.80) in the 1.5 M CsCl gradient, noise-free, from the closed form | 5 × 1 × 71 (*M*, *f/f₀*, v̄) | each band's v̄ to 10⁻⁴, shares 0.375 / 0.625 | 355 profiles, 0.2 s |

The contrast the gate reports for a gradient cell (19–100 (mL/g)⁻¹ above) is far beyond any D₂O
series (§3.4), and the fits show it: v̄ to the grid step from one cell.

**Cost.** An ASTFVM solve in a gradient is much dearer than an ASTFEM one: 0.16 s for the 2 h CsCl
test, ≈ 15 s for the 9.7 h AAV case (large *s*, bands a few hundredths of a centimetre wide). The
time goes to the finite-volume steps themselves — exponentials in the step, the gradient
interpolation, mesh refinement (sampled with a debugger) — not to one fixable hot spot. Band
forming steps at the band-forming table's interval (≥ 5 s). And `NO_DB` builds run serially (§3D).
For velocity data in a gradient that is the price; for an equilibrium scan the closed form removes
it.

### Real data: ABDE run 2098

AAV in PBS (1.00552 g/mL) loaded at 1.40 g/mL CsCl — 3.105 M by the component's density
polynomial on the PBS base — 48 krpm, 20 °C, the edit keeping the scan at 9.5 h; 61 wavelengths,
230–290 nm, each fitted on its own with the closed form (*M* 2–20 MDa, *f/f₀* held, v̄ 0.62–0.86
in steps of 0.0005: 1924 points, ≈ 1 s per wavelength), RI noise fitted. CsCl's *s* was set so its
equilibrium gradient is ω²*r*/β at the loading concentration (item 9 below): 0.393 S for
β = 1.25 × 10⁹, with 1.20 and 1.30 × 10⁹ as the bracket.

| Band (radius) | Density, g/mL, 61 wavelengths | Across β 1.20–1.30 × 10⁹ | Ideal neutral CsCl (1.06 S) | A₂₆₀/A₂₈₀ of its signal | Reading |
|---|---|---|---|---|---|
| upper (6.165 cm) | 1.3442 ± 0.0010 | 1.340–1.347 | 1.257–1.262 | 0.51 | protein: empty capsids |
| main (6.675 cm) | 1.3994 ± 0.0007 | 1.399–1.400 | 1.387–1.389 | 1.16 | DNA-loaded: full capsids |
| heavy shoulder (6.76–7.1 cm) | ≈ 1.433 | 1.431–1.435 | ≈ 1.48 | 1.32 | denser, DNA-rich particles |

The main band sits next to the cell's isoconcentration point, where the density is the loading
density whatever the gradient's steepness, so its density hardly depends on the CsCl model. The
upper band is half a centimetre away and moves with it. The data prefer the β-matched gradient
where the signal is strongest: at 230 nm (the upper band's peak is 0.59 OD) its RMSD is
5.2 × 10⁻³, the ideal neutral model's 1.8 × 10⁻². The shares are of absorbance at each wavelength,
not of particles: a particle ratio needs the empty and full capsids' extinction coefficients.

The closed form holds here. Refitting the 16 largest solutes of the 260 nm fit (55 % of its
signal) gives RMSD 1.787 × 10⁻² with the ASTFVM and 1.775 × 10⁻² with the closed form — the same
subset, so the unexplained rest costs both alike. And AAV-sized particles simulated with the ASTFVM
in this run's gradient (3.6 MDa at 1.344 g/mL, 5.4 MDa at 1.399) put the full band where the
closed form does, to 0.2 % of its peak; the empty band, nearer the meniscus where the gradient is
still forming, lags 0.006 cm behind its equilibrium place — about −0.001 g/mL in the fitted
density, inside the β bracket.

**s and D are not determined by this run, as modelled.** The early scans do carry them: at a fixed
5.4 MDa the full band at 2.7 h sits at 6.775, 6.715 or 6.679 cm for *s* = 60, 95 or 140 S, and at
9.5 h at 6.661 cm for all three. But an ASTFVM fit of all 14 scans (660 solutes: *s* 60–140 S,
*f/f₀* 1.0–1.6, v̄ in steps of 0.001 across the bands; 57 min on four threads, §3E threads) does
not reach the noise — RMSD 0.010–0.022 OD per scan against ≈ 0.003 — its solutes sit on the grid's
edges, and its band averages move with the grid (full capsids 62 S on an 11-value v̄ list, 71 ± 18 S
on this one; empty 91 and 102 ± 38 S, the wrong way round for AAV). The model fits any one late
scan; it is the sequence it cannot follow. The likeliest cause is the CsCl gradient's formation,
which the effective neutral solute of item 9 matches at equilibrium only; data effects add to it —
a feature fixed at 6.32 cm in every scan, already there at 42 min (optical, not a species), a
meniscus spike at 2.7 h, and the last scan's baseline correction applied to all. A sedimentation
velocity run in PBS measures *s* and *D* directly; from ABDE they need a gradient-former model that
is right in time, not only at equilibrium.

### Measured gradients

Item 9 below is not particular to CsCl: the solver sediments any gradient former as one dilute,
ideal solute. Run 1322 (polystyrene–polydopamine particles in sucrose, 40 krpm for 84 h, with
interference) shows it plainly — its sucrose gradient ends linear in *r*², the shape of a
concentrated, non-ideal former, and no single ideal solute fits the interference time course
(RMSD 1.3 fringes against ≈ 0.03 noise). Where the run measured its gradient, the model is not
needed: `US_FvmGradients::prepare_measured()` takes the former's molar concentration over the run
(the ported `CosedData` gains a constructor for a given table) and the ASTFVM interpolates it
exactly as it does a simulated one — identical simulations to 10⁻⁶ of the peak, and the engine
keeps such a gradient (`MeasuredGradientDrivesTheFitAsTheSimulatedOne`).
`us_3dsa_cli gradient-from-interference` makes the table from interference scans: a fringe fixes
a profile's shape, and conserving the former's mass at its loading concentration fixes its level.
(`--to-meniscus`/`--to-bottom` map it onto the absorbance data's cell, which the two optics place
up to 0.01 cm apart.)

**Cost near the isopycnic point.** The ASTFVM sizes its time step for the solute's *s* in the
buffer's own density — in a gradient buffer, the base buffer's. Run 1322's particles (about 400 S
in water, v̄ ≈ 0.93, banding at ≈ 1.07 g/mL) take 13 minutes per solve over its 90 hours. Sizing the
step for the largest *s* inside the gradient instead was 15 times faster and diverged — to ten
times the band's peak — since the mesh is built for the base-buffer *s*; that change is not kept.
A grid for such particles is a cluster job.

### Limits, and defects found in the ported code

For the branch's author; none is worked around here except where noted.

1. **Base buffer summed.** The cosedimenting 2DSA and the solver's internal fallback sum the base
   components' constant coefficient. The buffer editor gives every component the base buffer's
   density, so *n* base components give *n* × ρ. `US_FvmGradients` takes the mean, as
   `US_Math_BF` does.
2. **Only lamella excesses form a co-diffusing gradient.** `US_Math_BF` iterates over the overlay's
   excess components only ("for now iterate only over upper_cosed"). A lamella on a denser
   co-diffusing cell solution (the classic H₂O sample on D₂O) forms no gradient in the model, and
   since the editor's base excludes diffusing components, the D₂O's density is lost altogether.
3. `validate_csd()` never sets `co_sed`, so the solver's own fallback discards the salt data it
   computes; `salt_value == NAN` can never be true; `sqrt` of a slightly negative interpolated
   concentration gives NaN.
4. `InterpolateCCosed` can index one past the salt grid when the FVM mesh reaches beyond it.
5. `set_buffer()` allocates an empty `Band_Forming_Gradient` per call when there is no co-diffusing
   component, and never frees it.
6. `Band_Forming_Gradient::calculate_gradient` swaps the interpolation weights between scans and
   extrapolates rpm and ω²t with the temperature slope; it affects the tabulated temperature, and
   through it *D*, slightly.
7. Unconditional `qDebug` output (`fixed_dt`, `steps_per_transit`) per solve; `us_3dsa_cli` filters
   debug messages unless `US3DSA_DEBUG` is set.
8. The 2DSA's gradient debug dump passes density and viscosity in swapped order.
9. **A salt is simulated as one ideal, neutral solute.** `CosedData` sediments CsCl with a constant
   *s* and *D*, so its equilibrium gradient follows *M*(1 − v̄ρ)/*RT* for a single particle. CsCl
   is two ions and far from ideal; its real gradient is ω²*r*/β, β ≈ 1.2 × 10⁹. At 48 krpm,
   *r* ≈ 6.4 cm, that is 0.135 g/mL per cm, where *s* = 1.06 S, *D* = 2.0 × 10⁻⁵ cm²/s (the
   values *M*, v̄ and *D* imply) gives about 0.34. An effective *s* near 0.41 S brings it to
   0.125. On the ABDE run 2098 this choice moves the two bands' density difference from about
   0.12 to about 0.06 g/mL — as large an effect as the loading concentration. The component
   database's CsCl density coefficients are listed for 0–1.916 M; AAV ABDE loads near 3 M.

### Not done

* The desktop and MPI programs: they need the buffer's co-sedimenting components loaded with the
  solution and the ASTFVM selectable, exactly as the cosedimenting 2DSA does.
* Molar mass from ABDE: fit the approach to equilibrium (all scans, ASTFVM) — the hydrodynamics
  are in the early scans — or regularise toward the fewest bands.
* Validity of the closed form judged inside the engine rather than by `verify` in the CLI.

---

## 4. Design decisions

| # | Decision | Rationale |
|---|---|---|
| **D1** | 3DSA is a **global, multi-dataset** analysis. Single-dataset operation is refused (not merely warned) — unless the data set's buffer forms a density gradient (§3E), which the gate measures. | §3.3 — the fit is meaningless otherwise. Shipping a mode that silently returns a grid artefact would be worse than shipping nothing. A gradient cell spans its own range of densities, so the gate's contrast is real. |
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
| `us_solve_sim.h/.cpp` | **bug fix** [done] | The "vbar is constant" fast path was selected by `if ( attr_z != 3 )` — on the *position* of v̄ in the mask. With three genuinely varying axes that is wrong whenever v̄ lands in the z slot: dataset 0's cached corrections would be applied to every solute. Now `( attr_z != ATTR_V ) or fit_vbar`, identical to the old test whenever `fit_vbar` is false. |
| `us_solve_sim.cpp` | **bug fix** [done] | The same fast path took `vbartb`, `s20w_correction` and `D20w_correction` from `data_sets[0]` for *every* dataset in the loop. In a global fit each dataset has its own buffer and its own corrections; it now reads them from `dset`. See the note below. |
| `us_solve_sim.h` | **new field** [done] | `DataSet::fit_vbar` (bool, defaulted false). Set by the caller; drives the fix above. Also lets the existing 2DSA "vary vbar" mode state its intent instead of relying on attribute order. |
| `us_solve_sim.h/.cpp` | **bug fix** [merged] | Per-data-set TI and RI noise, by the review merged in `552482e`: `nnls_noise()` centres each data set's rows by its own means and solves NNLS on the projected design matrix (§3C). Applies to every caller, global 2DSA included. 3DSA uses it directly; the interim copy `US_SolveSimMDS` is retired. |
| `us_solve_sim.h/.cpp` | **new static** [done] | `buoyancy_contrast( QList<DataSet*>&, double vbar_mid, QString& msg )` returning the largest pairwise gain over the series, plus `vbar_resolution()` and the `VBAR_CONTRAST_REFUSE` / `VBAR_CONTRAST_WARN` thresholds. Evaluates at `SolutionData::density_tb`, per §3A. |
| `us_model.h/.cpp` | **new enum value** [done] | `THREEDSA` appended to `AnalysisType`; `"3DSA"` added to the `typeText()` map. |
| `us_solve_sim.h/.cpp` | **new field** [done, Phase 2] | `Simulation::scales`, one amplitude factor per data set, applied to each data set's simulated block before it reaches the A matrix (§6.4). Applied after the simulation cache takes its copy, so the cache only ever holds unscaled simulations. |
| `us_3dsa_process.h/.cpp` | **new** [done] | The engine (§3B). Since §3D it also gives every data set its own timestate before any solve (`prepare_timestates()`) and runs serially in `NO_DB` builds (`concurrent_solves_safe()`), reporting the thread count it used. Since §8.3 each fit carries a `US_SimCache` (`Parameters::use_cache`, on by default), so the amplitude iterations reuse the grid's simulations instead of solving them again. |
| `us_solve_sim.cpp` | *(not needed)* | `check_grid_size()` was listed for a 3-D extension, but it bounds Lamm-equation *time steps* from `s_max` alone. That is already dimension-independent. |
| `us_zsolute.h/.cpp` | *(no change)* | Already generalised; kept as the PCSA path. |
| `us_lamm_astfvm`, `us_math_bf`, `us_buffer`, `us_simparms`, `us_astfem_rsa`, `us_astfem_math`, `us_hardware`, `us_constants` | **ported** [done, §3E] | From the `cosedimenting` branch as they are (`4751867`); ASTFEM fits bit-identical. `US_Buffer::writeToDisk` fixed: co-sedimenting coefficients were written with empty names. |
| `us_fvm_gradients.h/.cpp` | **new** [done, §3E] | A data set's gradients, prepared once per fit and copied per thread; the density span for the gate; the density at a scan's time for the closed form. |
| `us_solve_sim.h/.cpp` | **new** [done, §3E] | ASTFVM dispatch from the standard-space solute; `buoyancy_contrast()` over gradient spans; `equilibrium_profile()` and `DataSet::equilibrium`. |

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
into the plan (§8.3). Run on the harness series, it agrees — but only once its input is corrected:
`us_density_match` as shipped cannot recover v̄ from the models `us_2dsa` writes (§8.3).

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

**Not to be confused with the simulation cache.** What the engine does reuse is exact: the
simulation of the same solute on the same data set, which every amplitude iteration after the first
asks for again (§6.4) — a simulation depends on the solute and the data set, never on the
amplitudes. Since §8.3 each fit carries `main`'s `US_SimCache`, keyed on values and returning
simulations exactly as computed. Over the 33 harness cases it cuts Lamm solves by 69 % and wall
time by 32 %, with bit-identical models.

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

Each pass is a full NNLS over the grid, but not a full set of Lamm solves: the simulations do not
depend on α, and the fit's simulation cache (§6.3) serves every pass after the first.

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
number of datasets, plus the amplitude iterations (7 measured, each a full NNLS pass; their Lamm
solves are paid once, since the fit's simulation cache serves every later pass — §6.3).

Measured throughput on the Phase 2 test geometry (4 scans × 261 points, 200 simpoints) was ≈ 1,200
Lamm solves per second across four threads. That figure came from the headless build, where
concurrent solves turned out to share work arrays (§3D); it stands as a rate, but the headless
engine now runs serially, about a quarter of it on four cores, and the desktop build's threaded
rate should be re-measured in Phase 3. A production geometry is one to two orders of magnitude
heavier per solve, which puts a full 64 × 64 × 16 three-dataset fit in the region of several hours
on four cores and tens of minutes on a cluster: **desktop-marginal, cluster-comfortable**. That is
now measurement-backed rather than estimated.

Two mitigations remain and both are real — the v̄ axis is intrinsically coarse (§3.4), and only
depth 0 is proportional to the grid size. A third, the coarse-then-refine v̄ scan of §6.3, is the
replacement for the cache and should be evaluated before the GUI settles its defaults.

Measured against `us_2dsa` on the harness series (§8.3), serially and before the cache: 3.6 ms of
wall time per Lamm solve against 2.2 ms. The difference is the NNLS — one problem over the whole
grid per amplitude pass at `grid_reps` 1, where the 2DSA solves 64-column subgrids — and with the
cache `grid_reps` 2 more than halves the same fit (case32 against case06, 29 s against 63 s). With the cache in place, the NNLS is now the larger cost; subgrids by default, or
amplitude passes over the survivors with one confirming pass over the full grid, are the levers.

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

* `test_us_solve_sim.cpp` — **from the review merged in `552482e`** (§3C). Covers the noise path
  3DSA depends on: `nnls_noise` leaves concentrations unchanged by fitted noise, is optimal for the
  projected problem, does not project Tikhonov rows, and gives each data set its own noise;
  `calc_residuals` removes TI and RI from ASTFEM simulations, including a global fit of two data
  sets of different sizes recovered to 10⁻⁹, and keeps columns aligned with Tikhonov rows before
  and after a norm cut.
* `test_us_3dsa_process.cpp` gained three checks with §3D:
  `PreparesATimestateOfItsOwnForEachDataSet` (one file and one speed profile per data set, and a
  data set that already has a timestate keeps it); `RunTimingMayDifferAcrossTheSeries` (a series at
  45, 50 and 38 krpm recovers v̄ — with the timestate preparation disabled it fails all three of its
  checks, residual 0.14); and `ThreadsDoNotChangeTheFit` (eight subgrids, four threads requested
  against one: the thread count used is reported, the fits are bit-identical, and the callers' data
  sets are left without a timestate). The engine's own test series is now generated with a timestate
  per run as well, so a `NO_DB` build cannot give the generated data a shared speed profile either.

### 8.2 Synthetic end-to-end — the harness

`test/3dsa_harness/` drives the full round trip: `us_3dsa_cli gen-cases` writes the case tree,
`us_astfem_sim` produces the data through its own CLI, and `us_3dsa_cli fit` fits each series and
reports the **global RMSD and the RMSD of every data set**. `test/3dsa_harness/README.md`
documents it; `run_harness.py --outdir DIR --bindir BINDIR` runs it.

Forty cases, covering every axis that matters:

| group | cases | what they exercise |
|---|---|---|
| Series length | 1–3 | two, three and five isotope concentrations |
| Loading | 4–5 | loadings spread 1.0 / 0.7 / 1.4 and 1.0 / 0.4 / 2.0 |
| Mixtures | 6–12 | species differing in *s*, in *D*, in v̄ and in combination; two and three components; mixtures sharing a v̄ while differing in *s* and *D* |
| Noise | 13–19 | random, time-invariant and radially-invariant noise, alone and together, with the fit asked to solve for TI and RI |
| Gates and edges | 20–24 | weak contrast and a single dataset, both of which **must be refused**; v̄ at both grid edges; a 1.5 S / 8.0 S mixture |
| Noise per cell | 25–28 | a **different noise level in every cell**, random and fitted, with loading varying against it |
| Irregular series | 29–31 | 0/30/50, 0/20/40/60 and 10/35/55/70/95 % D₂O — no pure cell at either end, uneven spacing |
| Solver state | 32–33 | eight subgrids with four threads requested, so a level has tasks to share out; and a series whose cells ran at **different speeds and run times** (45 krpm/5 h, 50 krpm/4 h, 38 krpm/6 h) — §3D |
| Crossing species | 34–40 | mixtures whose species **swap order in *s*** across the series — a pair, the same pair over five steps, a pair that co-sediments in one cell, with noise, at low signal, a five-species v̄ ladder — and a control that keeps its order; every truth off both methods' grids (§8.3) |

Every species is declared in standard (20W) space. `us_astfem_sim` converts to experimental space
per component using that component's own v̄ and the buffer it is given, and the fit has to invert
that. The round trip is the point — a harness that pre-converted would only be testing the NNLS.

Acceptance per case: recovered concentration-weighted v̄ and *s* within the case's tolerance,
fitted amplitude factors matching the imposed loading ratios, and the two negative controls
refused.

Cases 25–28 turn the aggregate RMSD cap and the spread check **off**, because a series whose cells
carry different noise has neither a meaningful global residual nor a meaningful spread: a 6×
spread is the correct answer there. Each data set carries its own cap instead, and the check is
that every cell's residual reaches the level *it* was given. Case 27 arranges for the noisiest cell
to also be the most weakly loaded, which is the worst case for an amplitude loop that confuses
signal with residual; it recovers loadings of 1.0 / 0.5 / 1.6 / 0.8 to four decimals.

Runs are thirty scans. Twelve resolved a boundary but left the fit short of the time information
that separates *s* from *D*, and the noise cases in particular were being asked to separate
systematic noise from the model on a dozen time points. Thirty cut case 16's residual four-fold
(8.0 × 10⁻⁴ → 1.9 × 10⁻⁴) and case 18's v̄ error five-fold (0.0082 → 0.0016).

Every other case fits one subgrid, so each of its levels is a single task and runs on one thread
whatever `--threads` asks for; case 32 is the only one in which the engine has work to share. In the
headless build that work still runs serially (§3D), and the report says so ("threads requested 4,
used 1").

Two caveats stand, both recorded in the harness README: D₂O density and viscosity are interpolated
linearly (harmless, since the same numbers reach both the simulator and the fit), and H/D exchange
is not modelled, so the recovered truth is cleaner than a real experiment's (§3.5).

### 8.3 Cross-check against `us_density_match`

On real density-series data, 3DSA's fitted v̄ and `us_density_match`'s post-hoc v̄ must agree within
their combined uncertainty. Disagreement is informative either way and should be understood before
release.

The harness now prepares its synthetic series for exactly this comparison. `us_astfem_sim` writes
more than the `.auc`: alongside it goes an experiment XML and an edit XML, and it registers a
buffer, one analyte per species and a solution tying them together under `$HOME/ultrascan/data`, so
a run loads as though it had come through `us_convert` and `us_edit`. The driver points `HOME` at a
directory inside `--outdir`, so a harness run neither reads nor disturbs a real installation, and
then fixes two things about what was registered:

* **The edit profile's radial range.** The simulator writes `left = meniscus + 0.0005 cm`, inside
  the rounding of the radial grid; `US_AstfemMath`'s interpolation calls `exit(-3)` — the process
  dies, it does not return an error — when the simulation grid does not reach the first edited
  radius. The driver rewrites the range to the same window `us_3dsa_cli` trims to, `[meniscus +
  0.02, bottom − 0.10]`, and moves the baseline and plateau inside it. Both analyses then see
  exactly the same radii, which is the only way their results are comparable.
* **The analytes' v̄.** They carry the value the data was generated from, which would hand a 2DSA
  analysis the answer 3DSA is being asked to find and leave `us_density_match` nothing to recover.
  The driver replaces it with a decoy 0.05 mL/g from the truth — in the solution's `commonVbar20`,
  in each of its `<analyte>` entries, and in each analyte file, since a reader may take any of
  them.

Each case then gets a `results/<case>.analysis.json` naming the work tree, each run's edit profile
and radial range, the records now carrying the decoy, and the truth they no longer mention.

The GUI programs themselves still cannot be built in a headless container (Qwt has no Qt6
package), but what they compute can be run.

**Run on the synthetic series (2026-10-25).** `US_2dsaProcess` depends on the GUI only through two
headers it includes and does not use, so a scratch driver ran it unmodified: one fit per cell at
the GUI defaults (s 1–10 S × f/f₀ 1–4 at 64 × 64, 8 grid repetitions, one iteration, the case's
noise flag), then a line-for-line port of `us_density_match`'s v̄ extraction — 51 boundary
fractions from 5 % to 95 %, simple D averaging, `US_Math2::linefit` of density against s at each
fraction, v̄ = 1/intercept, MW from the 0 % D₂O model. A second, independent port reproduces it to
10⁻⁹. Both methods saw the same `.auc` files, radii and timestates. Every distribution — 3DSA's,
DM's and the truth's — is read the way DM reads one, and a species' v̄ is the mean over the
boundary fractions in the inner 80 % of that species' interval. 31 series, 49 species:

| Input to `us_density_match` | Species v̄ error, median / max (mL/g) | Species MW error, median |
|---|---|---|
| s20w as `us_2dsa` writes them, analyte v̄ = the decoy | 0.31 / 14 | 53 % |
| … with the analyte carrying the true v̄ | 0.23 / 19 | 440 % |
| water declared as every cell's buffer | 0.073 / 0.12 | 55 % |
| the same s with the buoyancy correction undone | 0.0014 / 0.073 | 1.6 % |
| *3DSA, for comparison* | *0.0017 / 0.012* | *0.8 %* |

**`us_density_match` cannot recover v̄ from the models `us_2dsa` writes**, whatever v̄ the analyte
carries. The 2DSA's s20w is already corrected for buoyancy with the analyte's v̄ₐ, so across the
series

  s20w ∝ (1 − v̄ρ) / (1 − v̄ₐρ),

while DM fits ρ linear in s and reads v̄ from the intercept — exact only for s ∝ (1 − v̄ρ), that is
for v̄ₐ = 0. At v̄ₐ = v̄ the s20w are equal and the line is undefined; at any other v̄ₐ the pole at
ρ = 1/v̄ₐ throws the extrapolation far off. For case02 (true v̄ 0.73, v̄ₐ 0.68) the analytic
prediction is 0.426 and the pipeline gives 0.422; a sweep of v̄ₐ over 0.60–0.80 returns anything
from 0.35 to 8.9, or the 1.0 fallback. Declaring water instead removes the buoyancy factor but
also the viscosity correction, and D₂O's 25 % higher viscosity biases v̄ by +0.04 to +0.12. The fix
belongs in `us_density_match`, which already reads each component's `vbar20` and the buffer's
density and can undo the correction as it loads a model; it is drafted for upstream.

**Given the s its line assumes, DM agrees with 3DSA** — the cross-check this section asks for, and
it passes: the median error of the mean v̄ is 0.0006 for both, per species 0.0014 against 0.0017.
DM's tail is longer — slow, noisy species (case19's 2 S species, +0.073) and mixtures of equal s and
different v̄ (case30, +0.018), where 3DSA's worst over all 49 species is 0.012. DM also needs the
fine 2DSA grid: on 3DSA's own 19 × 9 *s* × *f/f₀* grid its median per-species error triples. And it
answers the weak-contrast series that 3DSA refuses (case20) without warning, 0.024 off even with
the corrected input.

**Species that change places (cases 34–40).** Every series above keeps its species in the same
order of *s* in every cell, which is what DM's pairing of the cells by boundary fraction assumes.
A species of high v̄ loses more of its buoyancy to D₂O than one of low v̄, though, and falls behind
it. Cases 34–39 are built to do that, with every *s*, *f/f₀* and v̄ off both methods' grids; case
40 is the control, as far apart in v̄ but in an order that holds. Against DM with its input
corrected, worst species v̄ error in mL/g and serial wall time:

| Case | 3DSA | DM | 3DSA s | DM s |
|---|---|---|---|---|
| 34 — pair, 4.2 S / 0.66 and 5.1 S / 0.805, swapping between 50 and 100 % D₂O | 0.007 | 0.081 | 40 | 26 |
| 35 — the same pair over five cells | 0.005 | 0.080 | 59 | 42 |
| 36 — a pair that sediments together in the 50 % cell | 0.056; 0.004 on a finer grid | 0.110 | 36; 64 | 25 |
| 37 — pair 34 with random, TI and RI noise | 0.005 | 0.077 | 37 | 25 |
| 38 — pair 34 at 0.10 / 0.08 OD, random noise 0.003 | 0.004 | 0.082 | 38 | 25 |
| 39 — five species, v̄ rising with *s* from 0.645 to 0.82 | 0.009 | 0.031 | 179 | 45 |
| 40 — control, order kept | 0.009 | 0.002 | 45 | 28 |

Once the order flips, DM's line runs through one species in H₂O and the other in D₂O. On exact *s*
values, case34's pair paired by boundary fraction gives 0.752 and 0.772 for a true 0.66 and 0.805,
so no grid can help. In cases 34, 35, 37 and 38 both species come back near the mean, with molar
masses 37–65 % off. 3DSA has no pairing to get wrong: there it recovers both species within 0.007
and their MW within 14 %.

Case36 defeats both on the harness grid, 3DSA because its two species share the 4.5 S grid point.
On a grid twice as fine in *s* and v̄ (25 × 13 × 21, two grid repetitions) it resolves them: within
0.0005 and 0.0044, MW within 6 %, in two populations with nothing between v̄ 0.69 and 0.79. The same
grid takes case34 to 0.0015 and 0.0038.

This is where 3DSA earns its place. For a mixture whose species keep their order in *s*, DM with its
input corrected is as accurate and faster; on the control it is the more accurate. For a mixture
whose v̄ differ enough to reorder its species — the heterogeneous complexes a density series is run
for — DM cannot be right, and 3DSA is about ten times more accurate per species.

**Cost**, serial throughout (§3D):

| | Wall time, 31 series | Lamm solves |
|---|---|---|
| DM pipeline, one 2DSA per cell | 949 s | 430,080 |
| 3DSA, re-simulating the grid every amplitude pass | 2,278 s | 642,540 |
| 3DSA with the simulation cache | 1,546 s | 197,505 |

Most of 3DSA's excess was simulating the whole grid again on every amplitude pass — case04's seven
passes cost 39,759 solves for a 3 × 1,881 grid. Each fit now carries a simulation cache (§6.3),
which brings the solves down to one per grid point and data set with bit-identical models in all
33 cases. 3DSA is still faster in only 8 of the 31: what remains is the NNLS (§7.2).

**Caveats.** Where the truth sits on a grid point the numbers flatter: 3DSA on case22 and case23,
and DM whenever the 2DSA was given the true v̄; the decoy runs are the representative ones, and
cases 34–40 keep every truth off the grids. 20 °C
only. The driver is not in the tree — it compiles the 2DSA engine with two GUI headers stubbed out,
which belongs in a test once that engine lives in `utils/`.

**What remains for Phase 5** is the comparison this section opened with — on real density-series
data, against `us_density_match` with the corrected input — and the physics synthetic data leaves
out, the mass of exchangeable hydrogens in D₂O, which shifts both methods.

### 8.4 Regression

2DSA and PCSA results must be bit-identical before and after the `us_solve_sim.cpp:723` fix and the
`Simulation::scales` addition — both are gated on new fields that default to the current behaviour.
Run the existing 2DSA test corpus and diff the output models.

That holds only for fits **without** TI or RI noise. Since the review merged in `552482e` (§3C),
any fit that asks for noise changes by design — a global fit because every data set is now
centred, a single-data-set fit because RI is now removed while the concentrations are fitted and
NNLS runs on the projected design matrix. Diff those models expecting a lower or equal residual,
not identity.

---

## 9. Phased delivery

| Phase | Content | Exit criterion | Est. |
|---|---|---|---|
| **0 — Spike** ✅ | `test_us_solve_sim_vbar.cpp` only: demonstrate the single-dataset degeneracy and two-density identifiability numerically. | **Done** — see §3A. Degeneracy exact to machine precision, confirmed through `US_Astfem_RSA`; §3.4 table reproduced; one correction found (`density_tb`). | 2 d |
| **1 — utils** ✅ | 3-D grid generation, mask validation, `buoyancy_contrast()`, `THREEDSA`, the `attr_z != 3` fix, `fit_vbar`. | **Done** — 27 tests green across `test_us_solute3d.cpp` and `test_us_solve_sim_vbar.cpp`; the full HPC target including `us_mpi_analysis` builds clean; both solver fixes are no-ops for existing single-dataset callers. A second latent bug found and fixed — see §5.1. | 3 d |
| **2 — Engine** ✅ | `US_3dsaProcess`, the parallel level runner, the amplitude loop. Driven headless from a test harness. | **Done** — see §3B. v̄ recovered to 1×10⁻⁴ mL/g and the loadings to four decimals on a synthetic three-buffer series; 7 engine tests green. The column cache was measured and withdrawn; two bugs outside 3DSA found and fixed. | 2 wk |
| **2a — Noise** ✅ | Every run of a series carries its own TI and RI noise. First as `US_SolveSimMDS`, a copy of the solver with data-set-aware noise; then fixed in `US_SolveSim` itself by the review merged in `552482e`, and the copy retired. | **Done** — see §3C. All 31 harness cases pass, including the six that fit noise; on the first fix the worst per-data-set RMSD on the TI case fell 45× and on the RI case 60×, and the merged solver fits noise-free data with injected TI and RI to machine precision. | 3 d |
| **2b — Solver safety** ✅ | Make the engine independent of the headless solver's process-wide state: a timestate per data set before any solve, serial solves in `NO_DB` builds. Two harness cases that exercise it. | **Done** — see §3D. A series with different run timings returns the right v̄ (0.7295, was 0.7923); eight subgrids at four threads no longer crash and match the serial fit bit for bit; all 33 harness cases pass. The upstream defects are reported, not fixed here. | 1 d |
| **2c — Cross-check** ✅ | §8.3 on the harness series: `us_2dsa` + `us_density_match` against 3DSA on the same data. The simulation cache it showed was missing. | **Done** — DM agrees with 3DSA once its input is corrected (species v̄ median 0.0014 against 0.0017) and cannot recover v̄ from `us_2dsa`'s models as shipped (median 0.31). Where species swap order in *s* across the series, even corrected DM misses by 0.03–0.11 and 3DSA is about ten times more accurate per species. The cache cuts 3DSA's Lamm solves by 69 % with bit-identical models. | 2 d |
| **3 — GUI** | `us_3dsa` main window + dataset-series manager, control panel, gate, plots, launcher registration. | An analyst can run the §8.2 case end to end from the menu. | 3 wk |
| **4 — MPI** | `3dsa_master`/`3dsa_worker`, parser, model writer, gate in the parser. LIMS coordination in parallel. | Cluster job completes and returns models equivalent to the desktop run. | 2 wk |
| **5 — Docs & integration** | `doc/manual/source/3dsa/`, help pages wired to `showHelp`, downstream consumer smoke tests, §8.3 cross-check on real data. | Manual builds; consumers verified; real-data cross-check documented. | 1 wk |

Phases 0–2 are the ones that can invalidate the concept, and they are deliberately front-loaded.
Phases 0, 1, 2, 2a, 2b and 2c have now run: §3.4 reproduced (§3A), the shared `utils/` layer in
place, the engine recovering v̄ from a synthetic series (§3B), every data set of a series carrying
its own systematic noise (§3C), the engine independent of the headless solver's process-wide state
(§3D), and the synthetic cross-check against `us_density_match` (§8.3). The design stands, with D7 withdrawn on measurement. All 40 harness cases pass. Phase 3 can
begin.

### Future work (explicitly out of scope here)

* **Per-dataset H/D exchange correction** for D₂O series (§3.5). The data-model hook is in Phase 1;
  the correction itself is a separate piece of physics work.
* ~~**Radial density gradient ρ(*r*)**~~ — done with the `cosedimenting` branch's ASTFVM (§3E):
  co-sedimenting and band-forming gradients, single-run v̄, and a closed form for ABDE scans.
  The desktop and MPI programs still need to load the buffer's gradient-forming components and
  offer the ASTFVM.
* Tikhonov regularisation tuned for the 3-D grid; Monte Carlo error bars on v̄.

---

## 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The degeneracy is not accepted and single-dataset 3DSA is requested anyway | Ships a number that looks like a measurement and is not | §8.1 test (2) as an executable guard; the gate refuses rather than warns; the manual states it plainly |
| LIMS-side work not scheduled | Cluster path unusable; desktop-only release | Raise at Phase 0; Phases 0–3 deliver a usable desktop program independently |
| Buffer ρ/η metadata inaccurate in real datasets | v̄ biased with the §3.4 gain | Series panel shows provenance of each value; cross-check §8.3 |
| Wall-clock worse than the §7.2 estimate | Desktop path impractical | Phase 2 measured it headless, before GUI investment; the simulation cache took the repeated Lamm solves out of the amplitude passes (§8.3). The NNLS is the remaining cost: subgrids by default, amplitude passes over the survivors, and the coarse-then-refine v̄ scan (§6.3) |
| `us_density_match` disagrees with 3DSA on real data | A correct 3DSA result looks doubtful, or a wrong DM one is believed | §8.3: as shipped, DM cannot recover v̄ from `us_2dsa`'s models — compare against DM with the buoyancy correction undone, and get the fix into `us_density_match` |
| The headless solver's process-wide state (§3D) is not fixed upstream | `us_3dsa_cli` stays serial, ~4× slower on four cores | Correctness does not depend on it: the engine prepares timestates itself and runs serially where unsafe. Lifting the cap is one function once `main` allocates per call in `NO_DB` builds |
| Some other shared state in the solver path that §3D's checks did not reach | Wrong fits only under threads, in the desktop build | `ThreadsDoNotChangeTheFit` compares a four-thread and a one-thread fit bit for bit in CI's desktop-style build |
| Copy-and-adapt duplication (`us_2dsa` → `us_3dsa`) drifts over time | Maintenance burden | Shared code pushed into `utils/` wherever it is genuinely shared; the duplication is confined to the scheduler and GUI, matching the existing `us_pcsa` precedent |

---

## 11. Summary of the answer to the original question

*Can we fit s, D and v̄ simultaneously, the way 2DSA fits s and D?*

**Not from one experiment in a uniform buffer** — the Lamm equation cannot see v̄, so the third axis
is unidentifiable and any value returned would be a grid artefact. **Yes from one experiment in a
density gradient** (ABDE, DGE, band forming onto CsCl; §3E), where each solute meets a range of
densities in its own cell.

**Yes from a buoyancy-contrast series** — and the existing UltraScan solver is already most of the
way there: it stacks datasets into one NNLS problem and already has a code path that varies v̄ per
solute with per-dataset corrections. The work is a 3-D grid generator, a scheduler with cubic
subgrid counts, per-dataset amplitude scaling, a GUI that manages the dataset series and refuses
under-determined fits, and the MPI plumbing to match. (A column cache to keep the cost down was
planned and withdrawn on measurement, §6.3; the third axis costs what it appears to cost. Exact
reuse of simulations across the amplitude passes is in, §8.3.)
