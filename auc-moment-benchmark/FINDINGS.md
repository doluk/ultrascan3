# Findings

Gate results, stated honestly, in the order the spec requires them to be
evaluated. Every number here is reproducible from `analysis/`.

## Summary

| Gate | Criterion | Result |
|---|---|---|
| **G0** | Weak-form and naive extraction agree on noise-free data | **PASS** |
| **G1** | `sigma(t)` scaling determined and reproducible | **PASS** — and the proposal's formula is wrong |
| **G2** | `N_eff >= 5` with realistic TI/RI noise | **FAIL as specified**; passes only under assumptions that are not met |
| **G3** | Crossover separation above ~5% vs 2DSA | **FAIL** — no crossover at any separation tested (margin inflated; see the asymmetry note) |
| **G4** | `BROAD`/`RA-fast` do not produce false clean rank gaps | **FAIL** — both produce confident false certificates |

**The spec says: stop at the first failure. That is G2, and G3 and G4 confirm
it independently.** G3 is the one that settles the project: there is no
separation at which the moment route beats a `c(s)`-style fit, even when the
moment route is handed advantages the real experiment cannot give it.

Two results are worth keeping regardless of the decision: the corrected
`sigma(t)` law (G1), and the observation that the weak form annihilates RI
noise exactly (§D.1).

---

## Before the gates: two constraints that reshape the method

### B.3 The observable window is far narrower than expected, and often empty

This is the single most consequential finding and it arrives before any noise
is injected.

The window plateau must cover the support **padded by `k sigma(t)`**, because
`g*` is the true distribution blurred by diffusion, and at realistic times
`sigma(t)` is *larger than the whole a priori support interval*. Required
`k = 4` (derivation and table in PHYSICS.md §3; `k = 2` costs 5–13% truncation
bias, `k = 4` costs <1e-3).

Sizing the window to the support alone — the obvious thing to do — is a trap:
it truncates the tails of `g*`, made our measured `sigma^2` **45x too small**,
and manufactured a spurious `rpm` dependence that looked like real physics.

Usable scans out of 100, `k = 4`, single species, `f/f0 = 1.25`:

| s \ rpm | 20k | 30k | 45k | 60k |
|---|---|---|---|---|
| 2 S | 0 | 0 | 0 | 0 |
| **4 S** | **0** | **0** | **0** | **2** |
| 7 S | 0 | 0 | 12 | 13 |
| 12 S | 0 | 16 | 16 | 12 |
| 20 S | 20 | 23 | 14 | 7 |
| 30 S | 30 | 19 | 9 | 4 |

**At 4 S there is no usable scan at any rotor speed.** The boundary never gets
4 diffusion widths clear of the meniscus before it reaches the bottom.
Lengthening the run does not help (the table is unchanged at 60 000 s).
4 S / ~52 kDa is the bread-and-butter `c(s)` case, so the method is unusable
exactly where the incumbent is most used.

Consequence for this study: the spec's 4 S test set is kept and documented as
failing, and a parallel **feasible set at 15 S** (`models.feasible_test_set`)
carries Phases C–E. Everything below is the 15 S set at 40 000 rpm, where
13–18 of 100 scans are usable. That is generous to the method.

### C.2 "Mean diffusion" is a real model error, not an approximation

Each species has its own `D` and therefore its own blur width, but the Hermite
deconvolution uses one `sigma`. On `F-M2-10` the residual against a
single-`sigma` model is **6.2% median, 8.2% max** — larger than the
white-noise moment error and comparable to the absorbance-optics error. The
proposal's "mean diffusion" assumption is in direct tension with its own claim
that each species carries its own `D` on the variety `V_q`.

---

## G0 — weak form vs naive extraction — PASS

By-parts and differentiate-then-integrate agree on noise-free `F-S1` to
**4.6e-6** at `m = 0`, rising to **2.9e-4** at `m = 8` (standard 1e-3 cm
radial sampling).

The residual is the *naive* estimator's finite-difference error, not a
weak-form error. Refining the radial sampling confirms it — the gap converges
away at close to third order, while the by-parts value is essentially
unchanged:

| radial resolution | 2.0e-3 | 1.0e-3 | 5.0e-4 |
|---|---|---|---|
| max rel. difference (m = 0..8) | 4.3e-3 | 2.9e-4 | 4.0e-5 |

So the by-parts algebra in PHYSICS.md §2 is right. The gap grows with `m`
because higher moments weight the steep part of the boundary more heavily,
which is exactly where a central difference is least accurate — another way
of saying the naive route is the one that fails first.

Supporting validation:

* **A.1** The finite-volume Lamm solver agrees with Faxén to 1.5e-2 at
  t = 2000 s falling to 5.8e-3 at t = 8000 s, on a plateau of ~0.95 (i.e.
  1.5% → 0.6%). The discrepancy *decreases* as the boundary clears the
  meniscus, which is the signature of Faxén's own error, not the solver's.
* **A.2** Mass is conserved to 5e-14. Time-step refinement converges cleanly
  (4e-3 → 4e-4 → 2.7e-5 → 1.6e-6 as `cfl` halves); the production setting
  sits at 1.6e-6, four orders below the 8e-3 white-noise floor.
* **A.2 against the real solver** — see the cross-validation section below.
  UltraScan's ASTFEM and `forward/lamm.py` agree to 0.01% of plateau, and
  their moment vectors agree to 1e-7 (`m=0`) through 6.6e-4 (`m=12`),
  which is >100x below the noise-induced error at every order. The spec
  asked for 10x. **A.2 passes.**
* Scharfetter–Gummel face fluxes are used rather than upwinding. Plain upwind
  injects numerical diffusion of order `v*dr/2`, which is indistinguishable
  from real diffusion and would corrupt every second and higher moment — the
  exact failure the spec warns about.

## Cross-validation against UltraScan's own solver

The Layer-1 generator was built and run (see "How Layer 1 was built" below),
so every result here now rests on two independent solvers rather than one.
`analysis/crossvalidate.py` reproduces this.

Concentration profiles, inside the observable window:

| model | usable scans | max abs difference | relative to plateau |
|---|---|---|---|
| F-S1 | 18 | 5.0e-5 | 0.01% |
| F-M2-10 | 15 | 4.4e-5 | 0.01% |
| F-M3 | 14 | 4.5e-5 | 0.01% |

Moment vectors, which is what actually propagates:

| model | m=0 | m=4 | m=8 | m=12 |
|---|---|---|---|---|
| F-S1 | 1.0e-7 | 4.9e-5 | 2.5e-4 | 6.6e-4 |
| F-M2-10 | 7.1e-8 | 5.0e-5 | 2.6e-4 | 6.6e-4 |
| F-M3 | 6.9e-8 | 5.1e-5 | 2.6e-4 | 6.8e-4 |

Against a white-noise relative error of ~7e-2 at `m=12`, solver disagreement
is more than **100x below the noise floor at every order**.

And G1 reproduces on ASTFEM output with a completely different numerical
method — finite element with a moving grid, against the finite-volume
Scharfetter-Gummel scheme in `lamm.py`:

| | exponent of `sigma^2` | measured / vHW prefactor |
|---|---|---|
| `forward/lamm.py` | -1.073 +- 0.004 | 1.090 |
| **UltraScan ASTFEM** | **-1.071 +- 0.003** | **1.087** |
| proposal | -3 | — |

No conclusion in this document is a solver artefact.

## G1 — the `sigma(t)` scaling — PASS, and the proposal is wrong

Measured exponent of `sigma^2` vs `t`, over 18 combinations of
`s in {10,15,22} S`, `f/f0 in {1.1,1.25,1.6}`, `rpm in {30,40,50} k`:

```
  exponent  =  -1.073 +- 0.004     (sd across all 18 conditions: 0.004)
  proposal  =  -3
```

The prefactor ratio to the `s*`-transform law

```
sigma^2(t) = 2 D / ( w^4 t r_b(t)^2 ),    r_b(t) = r_m e^{s w^2 t}
```

is **1.090**, stable over the whole grid (range 1.087–1.109). The residual 9%
is the sector-dilution/Faxén correction.

The proposal's `sigma^2 = 2D/(w^4 t^3)` is wrong by a factor `t^2/r_b^2`,
i.e. **4–6 orders of magnitude** at realistic times, and has the wrong
exponent. The measured `-1.073` rather than exactly `-1` is the exponential
growth of `r_b(t)`, predicted as `-1 - 2 s w^2 t`.

**WP1's "regression across `t^-3`" deliverable must be rewritten before a
mathematician builds a Hermite inversion on top of it.** The correct law is
the one van Holde–Weischet extrapolation already relies on.

## G2 — `N_eff >= 5` under realistic noise — FAIL as specified

`N_eff` here is the largest `m` for which the **total** relative error
`sqrt(Sigma_mm + bias_m^2)/|y_m|` stays under 10%. Quoting the standard
deviation alone is how a badly biased estimator looks excellent; see the
time-mean row below.

`m_max = 14`, 400 Monte Carlo realizations:

| model | white-only | absorbance | interference, TI left in | interference, **oracle** TI removal | interference, time-mean projection |
|---|---|---|---|---|---|
| F-S1 | 12 | 5 | **-1** | 12 | **-1** |
| F-M2-15 | 12 | 3 | **-1** | 12 | — |
| F-M2-10 | 12 | 4 | **-1** | 12 | **-1** |
| F-M2-05 | 12 | 5 | **-1** | 12 | — |
| F-M3 | 11 | 3 | **-1** | 12 | — |
| F-M2+AGG | 2 | 2 | **-1** | 2 | — |

`N_eff = -1` means even `m = 0` — the total loading concentration — is wrong
by more than 10%. With interference optics and TI noise left in, the relative
error at `m = 0` is **57–77%**. The method does not fail gracefully at high
moment order; it fails at order zero.

### D.1 Where the systematics actually go

* **RI noise is annihilated exactly.** The kernel integrates to zero because
  `W Phi_m` vanishes at both ends of the window, so any additive function of
  `t` alone drops out. Verified to 5e-10 relative. This is a genuinely nice
  property of the weak form and is worth keeping.
* **TI noise is not, and this is structural.** Subtracting the time-mean of
  the scans annihilates TI exactly and the *scatter* then looks superb — the
  same 8e-4 relative sd as white-noise-only. But it also removes part of the
  signal, so the estimand shifts: measured bias is **60% at `m=0` rising to
  ~2000% at `m=14`**. The estimator is precise and wrong.

  Removing TI without biasing the estimate requires estimating `psi(r)`
  (~1300 free parameters) from the data, which is only well-determined
  against the full 2-D scan set — i.e. it requires a model fit to all
  `n_t x n_r` points. That is precisely the inverse problem the moment route
  was supposed to sidestep. **TI removal cannot be done at the moment level.**

  The `oracle` column above subtracts the true TI vector. It is not
  achievable; it is the upper bound on what any TI-removal scheme could
  deliver, and it is quoted so the decision is made against the method's best
  possible case rather than its worst.

### D.2 The covariance is nearly rank-one

Across every model and noise case, the maximum absolute off-diagonal
correlation of `Sigma` is **0.997–1.000**. The moment errors are almost
perfectly correlated, because every moment order is computed from the same
handful of scans through kernels that differ only by a smooth power of `u`.

**An SDP or projection formulation assuming i.i.d. moment errors is solving a
different problem.** This is the strongest argument for handing over `Sigma`
rather than a scalar noise level, and it is the one thing in this package a
mathematician cannot reconstruct from the proposal.

### D.4 Contamination crossover

Fraction of `y_m` contributed by a 1% w/w aggregate:

| m | 0 | 2 | 4 | 6 | 8 | 10 |
|---|---|---|---|---|---|---|
| 4 S set, 20 S aggregate (5.0x) | 1.0% | 18.6% | 83.7% | 99.1% | 100% | 100% |
| 15 S set, 37.5 S aggregate (2.5x) | 1.0% | 5.4% | 24.3% | 64.0% | 90.7% | 98.2% |

The aggregate dominates from **m = 3** (5x separation) or **m = 6** (2.5x) —
at or below the low end of the spec's expected 5–8. Correspondingly
`F-M2+AGG` has `N_eff = 2` *even with white noise only*: 1% of contaminant
destroys everything above the second moment. Real samples are not pristine,
and `R_max = 0` is not a usable analysis.

## G3 — crossover vs `c(s)`/2DSA — FAIL

Median relative error in recovered `(s1, s2)`, 12 realizations, **absorbance
optics and oracle TI removal** — the moment route's best case. The comparator
is a `c(s)`-style NNLS fit over an s-grid of Lamm solutions at fixed `f/f0`
with TI/RI eliminated analytically (spec E.1 item 2).

| delta-s/s | c(s) NNLS `s1` | `s2` | Prony-from-moments `s1` | `s2` |
|---|---|---|---|---|
| 15% | 0.06% | 0.04% | 2.48% | 1.72% |
| 10% | 0.11% | 0.09% | 2.46% | 2.81% |
| 5% | 0.31% | 0.42% | 1.58% | 3.19% |
| 3% | 0.91% | 0.94% | 1.51% | 1.56% |

**The comparator wins at every separation** — by ~40x at 15%, ~25x at 10%,
~5x at 5%, ~1.6x at 3%. The gap narrows as the problem gets harder but never
closes, and the moment route's error is roughly flat at 1.5–3% because it is
limited by the blur model and the window, not by the separation.

There is no crossover above 5%, and none at 3%. Per the spec's gate table:
*no competitive advantage.*

### The comparison is not symmetric, and the asymmetry favours the comparator

This must be stated plainly, because it is the main threat to the conclusion
above.

**The NNLS comparator commits an inverse crime.** Its basis is built from
`lamm.solve_species` — the *same solver that generated the data*. It is
therefore fitting with a forward model that is exactly correct, which no real
analysis ever has. Its 0.06–0.9% errors are a floor no instrument could
reproduce.

The moment route does not commit that crime: `estimate_moments` touches the
solver nowhere. It applies the weak form to the raw scans and deconvolves
with the analytic blur law, so its 1.5–3% error is intrinsic to the method
rather than inherited from a perfect model.

Running against that, the moment route was given two pieces of oracle
knowledge the comparator was not:

* the true `D` values, used both to place the observable window
  (`windowing.observable_window` reads `D_max` from the truth) and to set the
  deconvolution width (`_representative` reads the concentration-weighted
  mean `D`). In practice both would have to be iterated;
* exact removal of the dominant systematic (the `oracle` TI mode), and
  absorbance rather than interference optics.

So neither side is being judged fairly, and the errors run in opposite
directions. What can and cannot be concluded:

* **Not trustworthy:** the *size* of the gap. A 40x margin at 15% separation
  is certainly inflated by the comparator's perfect forward model.
* **Still supported:** the *direction*. For the comparator to lose, model
  mismatch alone would have to degrade it by 3–25x, and `c(s)`/2DSA is known
  to resolve 10–15% separations routinely on real data. Meanwhile the moment
  route's 1.5–3% is set by the blur model and window truncation — both
  measured independently in C.2 and §B.3 — so it does not improve if the
  simulator improves.

An earlier draft of this document claimed the fix was to generate the data
with UltraScan's ASTFEM instead, so the comparator would no longer be fitting
its own solver's output. **That has now been tested, and it is not the fix.**
Re-running E.1 on ASTFEM-generated scans, with the NNLS basis still built
from `lamm.py`, leaves the comparator essentially unchanged:

| delta-s/s | c(s) on lamm data | c(s) on ASTFEM data |
|---|---|---|
| 10% | 0.11% / 0.09% | 0.10% / 0.08% |
| 5% | 0.31% / 0.42% | 0.31% / 0.35% |

The reason is the cross-validation above: the two solvers agree to ~1e-5 in
concentration, far below the noise, so which one generated the data is
immaterial. Swapping generators does not relieve the inverse crime.

The crime is therefore not "the comparator's Lamm solver wrote the data". It
is that **both solvers produce idealised Lamm solutions with no physical
model error** — no rotor wobble, no temperature drift, no non-ideality or
concentration-dependent `s`, no optical distortion. A basis built from clean
Lamm solutions matches such data almost exactly, and no amount of
cross-solver work changes that.

Settling the margin therefore needs one of: real experimental data, or
simulation with deliberate physical model error injected. Until then, **G3
should be read as "no evidence of an advantage", not as a measured margin.**
Note that the moment route is *not* similarly flattered — it never fits a
forward model at all — so model error would, if anything, widen the gap
rather than close it.

Note also that the proposal's Phase-1 milestone (15%, <1% error) is met by the
comparator at **0.06%**, and missed by the moment route at 2.5%.

## G4 — false rank certificates — FAIL

Hankel singular-value spectra, moments rescaled so `s` is in units of the
support midpoint (a pure scaling; shifting to `[-1,1]` needs a binomial
transform that loses ~14 digits and manufactures spurious singular values).

Noise-free truths behave exactly as they should: `F-S1` drops to 4.7e-17 after
`sigma_1`, `F-M2-10` after `sigma_2`, `F-M3` after `sigma_3`, and the
non-atomic `F-BROAD` decays smoothly with no gap at all.

The estimated spectra do not. Largest gap ratio `sigma_k/sigma_{k+1}`, and the
`k` at which it falls — that is, the rank you would certify:

| model | true R | certified R | gap |
|---|---|---|---|
| F-S1 | 1 | **1** | 937x |
| F-M2-10 | 2 | 5 | 888x |
| F-M3 | 3 | 5 | 376x |
| **F-BROAD** | **none** | **2** | **144x** |
| **F-RA-fast** | **none** | **3** | **260x** |

Four of five are wrong. Both non-atomic distributions return a *confident*
rank certificate — `F-BROAD`'s apparent rank-2 gap (144x) is larger than the
genuinely rank-2 `F-M2-10`'s gap at `k = 2` (32x). A log-normal peak and a
rapid monomer–dimer equilibrium — the system people most want to analyse —
both certify a rank they do not have.

This is the method's worst failure mode and it is not a corner case.

---

## Open items

Things this prototype did **not** do, which a reader should not assume were
done:

1. **Layer 1 is built, run, and cross-validated. (Closed.)**
   `generate/generate.cpp` now compiles against real UltraScan `libus_utils`
   and `US_Astfem_RSA`, and the data it produces agrees with
   `forward/lamm.py` to >100x below the noise floor at every moment order
   (see the cross-validation section). Compiling it caught a member this file
   had invented from memory (`US_Model::compressibility`, which does not
   exist) and a speed-profile setup that left `avg_speed`, `set_speed` and
   the omega^2t values unpopulated, which made the solver emit NaNs and write
   nothing.

   Two caveats on the build, both recorded in `generate/CMakeLists.txt`:
   it builds only `libus_utils` in the no-database configuration rather than
   going through the top-level CMakeLists (which pulls in OpenGL, qwt,
   qwtplot3d and X11 for the GUI); and it was built against Ubuntu's Qt 6.4.2
   rather than the Qt 6.9 the project pins, which needs the small shim in
   `generate/qt_compat.h`. The pinned toolchain image
   (`ghcr.io/ehb54/us3-toolchain-ubuntu2404`) could not be pulled here:
   `ghcr.io` is reachable but the layer host
   `pkg-containers.githubusercontent.com` is refused by this environment's
   egress policy. Building inside that image would remove the shim.

2. **The 2DSA comparator is a stand-in**, not `us_2dsa`. It is the right
   *kind* of baseline (the `c(s)` analogue with TI/RI eliminated, recovering
   both peaks of `F-M2-10` to 0.02% on noise-free data) and it now runs
   against ASTFEM-generated data, but it is still NNLS over a Lamm basis
   rather than UltraScan's own 2DSA implementation, and it still fits data
   that carries no physical model error (see the G3 asymmetry note). Running
   the real `us_2dsa`, `us_dcdt` and `us_vhw_enhanced` on the `.auc` files
   the generator now writes is the remaining unfinished comparison.

3. **TI/RI vectors are synthetic.** They are built to have the right character
   — smooth window distortion plus narrow scratches, not white — but real
   vectors harvested from instrument runs via `US_Noise` would make the study
   materially more defensible. `noise.load_us_noise` reads them; it has not
   been exercised against a real file.
4. **`B.4`, the exact Lamm weak form**, is derived in PHYSICS.md §6 but not
   implemented as a running estimator.
5. `F-RA-fast` has only 4 usable scans, so its Phase-D numbers are thin. Its
   G4 result does not depend on that (it is a single-realization spectrum).

## How Layer 1 was built

For the record, since the environment mattered:

* The pinned toolchain image `ghcr.io/ehb54/us3-toolchain-ubuntu2404` is
  public and its manifest resolves, but its layers are served from
  `pkg-containers.githubusercontent.com`, which this environment's egress
  policy refuses (403 at CONNECT). The image could not be pulled.
* The generator was instead built against Ubuntu 24.04's Qt 6.4.2, compiling
  only `libus_utils` with `NO_DB` (the HPC profile's subset), which avoids
  the GUI dependency chain entirely.
* UltraScan targets Qt 6.9. The one incompatibility that bites under 6.4 is
  comparing the `QStringView` from `QXmlStreamReader::name()` against a
  string literal, which the utils XML readers do in ~146 places.
  `generate/qt_compat.h` supplies the missing operator and is force-included
  into the `us_utils` compilation only, so **no upstream source file was
  modified**. It compiles to nothing on Qt >= 6.5.

Reproduce with:

```
cd generate && cmake -S . -B build && cmake --build build -j
./build/generate /tmp/us3out
python3 ../analysis/crossvalidate.py /tmp/us3out
```

## What is worth keeping

If the project stops here, three things are still worth writing up:

* The corrected `sigma(t)` law, measured to 0.4% across 18 conditions.
* The exact annihilation of radially-invariant noise by any compactly
  supported weak form — useful to anyone computing moment-like functionals of
  AUC scans, independently of this inversion scheme.
* The cross-validation itself: an independent, dependency-free reference
  solver that agrees with ASTFEM to 0.01% of plateau is a useful regression
  check for UltraScan in its own right.
* The observable-window constraint of §B.3, which applies to *any* method
  that works in `s*`-space and needs unbiased high moments, and which appears
  not to be stated anywhere in the `g*(s)` literature.
