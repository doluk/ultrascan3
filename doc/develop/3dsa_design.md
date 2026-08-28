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

## 4. Design decisions

| # | Decision | Rationale |
|---|---|---|
| **D1** | 3DSA is a **global, multi-dataset** analysis. Single-dataset operation is refused (not merely warned). | §3.3 — the fit is meaningless otherwise. Shipping a mode that silently returns a grid artefact would be worse than shipping nothing. |
| **D2** | Primary grid is (*s*<sub>20,W</sub>, *f/f₀*, v̄<sub>20</sub>). (*s*, *D*, v̄) offered as an alternate axis mapping. | *f/f₀* has a bounded, physically meaningful range (1–4) that is independent of *s*. A rectangular *s*×*D* grid spends most of its points on *f/f₀* < 1 — physically impossible shapes. *D* is reported as a derived output, so "fitting *s*, *D* and v̄" is satisfied either way. `US_Model::calc_coefficients()` supports the "s and D" input branch directly (`utils/us_model.cpp:224`), so the alternate mapping is nearly free — add an *f/f₀* ≥ 1 filter to drop non-physical points. |
| **D3** | New program `programs/us_3dsa/`, not a mode of `us_2dsa`. | (a) The GUI needs a multi-dataset series manager that `US_AnalysisBase2` does not provide; (b) 2DSA is GMP-validated and heavily used — a third axis in its control panel is a regression risk; (c) the MPI dispatch keys off `analysis_type`. Matches the existing `us_pcsa` precedent. |
| **D4** | Reuse `US_SolveSim::calc_residuals()` as-is for the inner solve; share grid generation and the identifiability metric via `utils/`. | The `stype > 9` path already does exactly the right thing per dataset. Duplicating a 1,800-line solver would be the single largest source of divergence bugs. |
| **D5** | Add per-dataset amplitude scale factors, fitted in an alternating outer loop. | §3.6. Small, well-defined, and removes a bias that would otherwise corrupt the headline number. |
| **D6** | Buoyancy-contrast gate in the GUI *and* in the MPI parser. | Cluster jobs bypass the GUI; the gate has to live where the fit starts, in both paths. |
| **D7** | Column cache keyed on quantised (*s\**, *D\**), per dataset. | §6.3. This is the main cost mitigation and it follows directly from §3.1: the simulation depends on nothing else. |
| **D8** | `US_Model::THREEDSA` appended at the **end** of `AnalysisType`. | The enum is serialised as a bare integer in model XML (`utils/us_model.cpp:707-712`). Inserting in the middle would silently reinterpret every stored model. |

---

## 5. Component inventory

### 5.1 `utils/` — shared by GUI and MPI

| File | Change | Detail |
|---|---|---|
| `us_solute.h/.cpp` | **new API** | `init_solutes_3d( xlo, xhi, nx, ylo, yhi, ny, zlo, zhi, nz, grid_reps, s_mask, out )` producing `grid_reps³` interleaved subgrids. Writes values into the `US_Solute` field selected by the mask, matching `US_SolveSim::set_comp_attr()` conventions (`ATTR_S`→`.s`, `ATTR_K`→`.k`, `ATTR_V`→`.v`, `ATTR_W`/`ATTR_D`/`ATTR_F`→`.d`). |
| `us_solute.h/.cpp` | **new API** | `validate_mask( s_mask, QString& err )` — rejects any mask that puts two axes in the shared `.d` slot (e.g. *D* and *MW* together), and rejects duplicate attributes. |
| `us_solve_sim.h/.cpp` | **bug fix** | `utils/us_solve_sim.cpp:723` selects the "vbar is constant" fast path with `if ( attr_z != 3 )` — i.e. on the *position* of v̄ in the mask. With three genuinely varying axes this is wrong whenever v̄ lands in the z slot: it would apply dataset 0's cached corrections to every solute. Replace the positional test with an explicit flag. |
| `us_solve_sim.h` | **new field** | `DataSet::fit_vbar` (bool). Set by the caller; drives the fix above. Also lets the existing 2DSA "vary vbar" mode state its intent instead of relying on attribute order. |
| `us_solve_sim.h/.cpp` | **new field** | `Simulation::scales` — per-dataset amplitude factors (§6.4), defaulting to all-ones so existing callers are unaffected. |
| `us_solve_sim.h/.cpp` | **new static** | `buoyancy_contrast( QList<DataSet*>&, double vbar_mid, double& dlnR_dvbar, QString& msg )` — the §3.4 metric, used by the GUI gate and the MPI parser. |
| `us_solve_sim.cpp` | **extend** | `check_grid_size()` messages and limits for 3-D point counts. |
| `us_model.h/.cpp` | **new enum value** | `THREEDSA` appended to `AnalysisType`; `"3DSA"` added to the `typeText()` map. |
| `us_zsolute.h/.cpp` | *(no change)* | Already generalised; kept as the PCSA path. |

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

### 6.3 Column cache keyed on (*s\**, *D\**) — the main cost mitigation

Follows directly from §3.1: within one dataset, the simulated column depends on *nothing but*
(*s\**, *D\**). The 3-D grid maps onto a 2-D manifold per dataset, so a large fraction of grid
points request simulations that are indistinguishable from one already computed.

* Quantise (*s\**, *D\**) at the resolution below which the simulated curves differ by less than a
  fraction of the data noise; use that as a hash key.
* Cache per dataset, per worker thread, for the lifetime of one task.
* Across the series the *stacked* column is still unique — which is exactly why the fit works — but
  each *block* can come from the cache.

Expected saving: 3–10× on Lamm-solver calls, concentrated precisely on the degenerate work. The
quantisation tolerance must be a tunable with a conservative default and must be reported in the
run log, because it is a genuine approximation.

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

For the loaded series and the midpoint of the requested v̄ range, compute for every pair (*e₁*, *e₂*)

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
2. **The column cache (§6.3)** removes 3–10× of that, and removes exactly the redundant part.
3. **Subgridding already bounds the working set.** At `grid_reps` = 8 the 3-D grid yields 512
   subgrids of 128 points — comparable to the 64 subgrids of 64 points that 2DSA handles today. Per
   task, the NNLS matrix stays the same order of magnitude; what grows is the *number* of tasks,
   which is the parallelisable dimension.

Net expectation: **2–5× the wall-clock of a 2DSA run per dataset**, times the number of datasets in
the series. For a 3-dataset series that is roughly 6–15× a single 2DSA run — well inside what the
cluster path handles routinely, and at the upper end of, but not beyond, what a desktop run can do
overnight.

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

* `test_us_solute3d.cpp` — grid point counts; subgrid partition covers the box exactly once;
  `grid_reps` = 1 degenerates to a single full grid; mask validation rejects (*D*, *MW*) pairs and
  duplicate attributes; the *f/f₀* ≥ 1 filter on the (*s*, *D*, v̄) mapping.
* `test_us_solve_sim_vbar.cpp` — **encodes the physics as a test**:
  1. (*s*, *f/f₀*, v̄) and (*s*, *D*, v̄) parameterisations of the same species yield identical
     (*s\**, *D\**);
  2. for a **single** dataset, distinct grid triples produce columns whose pairwise angle is below
     tolerance — i.e. the degeneracy is real and detected;
  3. for a **two-density** series, the same triples produce well-separated stacked columns;
  4. `buoyancy_contrast()` reproduces the §3.4 table to the stated precision.

  Test (2) is the guard that stops anyone from "helpfully" re-enabling single-dataset 3DSA later.

### 8.2 Synthetic end-to-end

Generate three datasets with `us_astfem_sim` from a known two-species model at ρ = 0.998, 1.05,
1.10, with (deliberately) different loading concentrations. Run 3DSA. Acceptance: recovered v̄
within the δv̄ predicted by §6.5 for the imposed noise level; recovered *s* and *D* at least as
accurate as a per-dataset 2DSA; fitted α<sub>e</sub> matching the imposed loading ratios.

Add a negative control: the same fit on a series with ρ spread of 0.01, which must be **refused**
by the D6 gate.

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
| **0 — Spike** | `test_us_solve_sim_vbar.cpp` only: demonstrate the single-dataset degeneracy and two-density identifiability numerically. | The numbers in §3.4 confirmed against the real solver. De-risks everything downstream and produces the documentation figures. | 2 d |
| **1 — utils** | 3-D grid generation, mask validation, `buoyancy_contrast()`, `THREEDSA`, the `attr_z != 3` fix, `fit_vbar`. | Unit tests green; 2DSA/PCSA regression clean. | 3 d |
| **2 — Engine** | `US_3dsaProcess`, `WorkerThread3D`, column cache, scale-factor loop. Driven headless from a test harness. | Synthetic 3-dataset fit (§8.2) recovers v̄ to spec, without any GUI. | 2 wk |
| **3 — GUI** | `us_3dsa` main window + dataset-series manager, control panel, gate, plots, launcher registration. | An analyst can run the §8.2 case end to end from the menu. | 3 wk |
| **4 — MPI** | `3dsa_master`/`3dsa_worker`, parser, model writer, gate in the parser. LIMS coordination in parallel. | Cluster job completes and returns models equivalent to the desktop run. | 2 wk |
| **5 — Docs & integration** | `doc/manual/source/3dsa/`, help pages wired to `showHelp`, downstream consumer smoke tests, §8.3 cross-check. | Manual builds; consumers verified; cross-check documented. | 1 wk |

Phases 0–2 are the ones that can invalidate the concept, and they are deliberately front-loaded:
**if Phase 0 does not reproduce §3.4, the rest of the plan should be reconsidered before any GUI
work begins.**

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
