# Rotational cell misalignment: model and prototype

A misaligned cell is turned in the rotor hole by an angle δ about its own
axis, so its sector walls are no longer radial. This directory has the
reference implementation of the model, in Python, and the parameter study.
The model also runs in the ASTFVM solver (`US_LammAstfvm`) through
`US_CellMisalignment` (`utils/us_cell_misalign.{h,cpp}`).

## 1. Literature (summary)

| Source | Finding |
|---|---|
| Arthur et al. 2009, *J Pharm Sci* 98:3522 (PMID 19130472) | Cell alignment is the largest source of variability in aggregate quantitation by SV-AUC. |
| Gabrielson et al. 2009, *Anal Biochem* 396:231 (PMID 19782040) | Multi-lab precision study; lists alignment as a precision factor. |
| Doyle et al. 2017, *Anal Biochem* 531:16 (PMID 28529050) | An optical alignment system improves the precision of aggregate quantitation. |
| Channell et al. 2018, *Eur Biophys J* 47:809 (PMID 30159596) | BSA misaligned by ±1°, ±2°, ±4°. Monomer and dimer peaks in c(s) broaden. The dimer s falls; the monomer s does not change. |
| Khasa et al. 2021, *Mol Ther Methods Clin Dev* 21:585 (PMID 34095342) | A 4° misalignment raises the BSA aggregate fraction from 16 % to 20 % in SV. Band-forming centrifugation is insensitive to ±5°. |
| LeBrun et al. 2018, *PLoS One* 13:e0201529 (PMID 30059530) | Scribe-line alignment is typically within 0.5°. Gives the optical registration error of a rotated pattern. |
| Vezočnik et al. 2025, *J Pharm Sci* 114:103977 (PMID 40882870) | Checks alignment inline from the instrument's own scans. |
| Luo et al. 2026, *Eur Biophys J* 55:639 (PMID 41830968) | Alignment effects in AAV SV-AUC. Content not verified. |
| Schachman 1959; To et al. 2019, *Anal Chem* 91:5866 | Qualitative: non-radial walls produce lateral density gradients and convection. |

No quantitative or mechanistic model of sedimentation in a misaligned cell was
found: no Lamm-equation extension and no CFD. All evidence is empirical
(c(s) output).

## 2. Model

### 2.1 Geometry (exact)

Take a sector of half-angle α whose channel centre line is at polar angle β,
and rotate it by δ about a pivot at radius r_p, the cell centre (6.5 cm in
An-50/An-60 rotors).

- **Wall line.** The wall that was at polar angle φ₀ = β ± α now runs at
  angle φ₀ + δ and lies at distance
  `p = r_p [sin(φ₀+δ) − sin φ₀] ≈ r_p δ` from the rotor axis.
- **Wall tilt.** At radius r the wall makes an angle ψ with the radial
  direction, with `sin ψ(r) = p / r`. For δ = 1° at r = 6.5 cm, ψ ≈ 1.00°.
- **Channel width.** The angular width changes only at O(αδ²), so the radial
  cross-section A(r) and the radial dilution are those of the aligned cell.
  The meniscus stays at r = const because it is an iso-potential surface.
- **Off-centre channels** (β ≠ 0) shift radially by `−δ r_p sin β`. That is
  −30 µm for δ = 1° and β = 1.5°.
- **Flat bottom.** The tilted bottom face spans `2α r_b sin δ` radially:
  55 µm per degree.
- **Optics.** The beam at the nominal centre line stays more than 1.2 mm from
  the walls for δ = 1°, so there is no clipping. Absorbance optics see c(r)
  only, so misalignment adds no optical smearing in the ideal case.

### 2.2 Transport closure

- **Wall layers.** At a tilted wall the wall-normal sedimentation flux
  c v sin ψ has to be balanced by diffusion. This leaves a depleted layer on
  the "upstream" wall and an enriched layer on the other wall.
- **Why they move.** A lateral density anomaly in a centrifugal field has no
  hydrostatic equilibrium. The depleted layer rises to its neutral level,
  which is inside the boundary. The enriched layer slides to the bottom.
- **Analogy.** This is the Boycott effect, described kinematically by
  Ponder (1925), Nakamura & Kuroda (1937), and Acrivos & Herbolzheimer (1979)
  (PNK theory).
- **Coriolis.** The flow is planar and perpendicular to Ω, so the Coriolis
  force is a pure gradient and does not suppress it.

Averaged over the cross-section, this gives a 1D equation:

```
∂c/∂t = −(1/r) ∂/∂r [ r ( s ω² r c − D ∂c/∂r ) ] − u(r) ∂c/∂r
u(r)  = κ s ω² p (b − r) / (Φ r)          (Φ = angular width, b = bottom)
```

The solute removed by the last term is deposited at the bottom. Solvers use
the equivalent conservative form:

```
v_eff = s ω² r (1 + ε(r)),   ε(r) = κ p (b − r) / (Φ r²)
sink  = −k(r) c,             k(r) = κ s ω² p / (Φ r)
```

- **Sharp-boundary limit.** For a sharp boundary the PNK law follows:
  `dr_b/dt = s ω² r_b [1 + κ p (b − r_b)/(Φ r_b²)]`.
- **Plateau.** The plateau dilutes radially exactly as in the aligned cell.
- **κ ∈ [0, 1]** is the transport efficiency of the wall layers. κ = 1 is the
  convection-dominated limit; κ = 0 is the ideal aligned cell.
- **Time scales.** The 2D probe (`misalign2d.py`) shows buoyancy-driven
  velocities of about 0.1 cm/s, roughly 10³ times the sedimentation velocity.
  This time-scale separation is what justifies the quasi-static closure.

## 3. Prototype

| File | Content |
|---|---|
| `misalign1d.py` | Geometry; a 1D sector Lamm solver (Scharfetter–Gummel finite volume) with the closure; least-squares fitting with the ideal model. |
| `study.py` | Fits misaligned-cell data with the aligned model to get apparent s, D and dimer fraction versus δ and κ. Writes `results.json`, `fig_bias.png` and `fig_profiles.png`. |
| `misalign2d.py` | Exploratory 2D Stokes–Boussinesq–Lamm model in a tilted channel. It is stiff (see its header) and is kept only to document the time scales. |
| `misalign2d_implicit.py` | Implicit 2D Stokes–transport solver (section 5). |
| `analysis2d.py`, `plot2d.py`, `plot2d_summary.py` | Fits of the 2D runs with the 1D model; figures. `results2d.json` holds the fit results. |
| `utils/us_cell_misalign.*` | C++ geometry and closure. |
| `utils/us_lamm_astfvm.*` | ε(r) applied in `AdjustSD` (all non-ideal cases). The sink plus bottom deposit is applied by operator splitting in `ApplyMisalignSink`. |
| `utils/us_simparms.*` | New fields `misalign_angle` (deg, default 0), `misalign_pivot` (cm, default 6.5) and `misalign_kappa` (default 1). They are written to and read from simparams XML only when the angle ≠ 0, and are included in the `US_SimCache` key. |
| `test/utils/test_us_cell_misalign.cpp` | Unit tests: geometry against the Python reference, XML round trip, κ = 0 identical to the aligned cell, ASTFVM boundary shift against the PNK law (3 %), plateau unchanged. |

How to use it: set `simparams.misalign_angle` (and optionally `misalign_pivot`
and `misalign_kappa`) and select the ASTFVM mesh type. ASTFEM ignores the
misalignment and prints a warning.

### Verification
- **Python solver.** Mass is conserved to 1e-15. The boundary midpoint
  matches the analytic PNK trajectory to within one grid cell (3 µm).
- **C++ against Python.** The C++ ASTFVM and the Python solver agree on the
  boundary midpoint within 0.3–3 µm (δ = 1°, κ = 1, 50 krpm, BSA).
- **C++ against PNK.** The excess shift in C++ matches the PNK prediction to
  within 1 %.

## 4. Results (BSA, 50 krpm, r_m = 6.0, b = 7.2 cm, 2.5° sector, 3 h)

See `results.json`, `fig_bias.png` and `fig_profiles.png`.

Preliminary (κ = 1; true values s = 4.4 S, D = 6.0e-7 cm²/s, dimer fraction 15 %):

| δ (°) | s_app (S) | D_app (1e-7) | rmsd | 2-species fit: s₁ / s₂ / f₂ |
|---|---|---|---|---|
| 0    | 4.400 | 5.98 | 6e-5   | 4.401 / 6.802 / 0.150 |
| 0.25 | 4.464 | 5.92 | 1.7e-3 | 4.470 / 6.903 / 0.146 |
| 0.5  | 4.528 | 5.85 | 3.4e-3 | 4.536 / 6.999 / 0.143 |

The full table for δ up to 4° and κ ∈ {1, 0.1} is produced by `study.py`.

## 5. Implicit 2D Stokes–transport solver

`misalign2d_implicit.py` solves the 2D model (planar, curl-free tilted
field; see `misalign2d.py` for the equations). It omits sector divergence,
which is the same for the aligned and the misaligned cell to O(α).

- **Time stepping.** Linearly implicit backward Euler of the coupled
  (c, Ψ) system. Newton–Picard iterations act on the bilinear flux c·u(Ψ) and
  on a minmod second-order correction, converging in 4–6 iterations to a
  residual below 1e-6.
- **Linear algebra.** One sparse solve per iteration, using PARDISO when
  available.
- **Analysis (`analysis2d.py`).** Profiles are averaged along lines of
  constant radius (`--center`: the channel centre line, i.e. the absorbance
  beam). They are fitted with the ideal planar 1D model (s, D free), and
  alternatively with the closure (s, D, κ_a, κ_d free). Effects are quoted
  relative to the aligned run on the same grid, which removes the solver's
  numerical diffusion.

Setup: BSA (4.4 S, 6.0e-7 cm²/s), 50 krpm, r_m = 6.2 cm, L = 0.9 cm,
W = 2.8 mm, t = 4000 s, grid 120×40, dt = 5 s.

| ψ | c0 (mg/mL) | Δs/s | ΔD/D | rmsd (aligned: 1.3e-3) |
|---|---|---|---|---|
| 1° | 0 (no flow) | +0.02 % | +0.4 % | 1.4e-3 |
| 1° | 0.001 | +0.68 % | +9.2 % | 8.1e-3 |
| 1° | 0.01 | +0.45 % | +5.7 % | 6.2e-3 |
| 1° | 0.1 | +0.25 % | +3.1 % | 4.1e-3 |
| 2° | 0.01 | +0.91 % | +10.9 % | 1.0e-2 |

Numerical checks at ψ = 1°, c0 = 0.01:

| check | Δs/s | ΔD/D |
|---|---|---|
| grid 180×60 | +0.55 % | +6.7 % |
| dt = 10 s | +0.40 % | +8.1 % |
| dt = 2.5 s | +0.48 % | +4.5 % |
| dt → 0 (Richardson extrapolation, first order) | ≈ +0.51 % | ≈ +3.3 % |
| centre-line signal instead of lateral mean | +0.45 % | +5.4 % |

Figures: `fig2d_field.png` (field and profiles at 0.01 mg/mL) and
`fig2d_summary.png` (effect versus c0).

**Findings**

1. **The flow causes the artifact.** Sedimentation–diffusion alone in a
   tilted channel leaves wall layers (5 % lateral rms) but no measurable bias.
   The artifact comes from the buoyancy-driven flow these layers set up, which
   reaches 10–100× the sedimentation velocity even at 0.001 mg/mL.
2. **The signature is broadening, not an s shift.** At dt = 5 s, ΔD/D is
   about 13× Δs/s; extrapolated to dt → 0 it is about 7×. Both are roughly
   linear in ψ. Misfit with a single ideal species is 3–6× the
   aligned baseline. Qualitatively this matches the experiments: broadened
   c(s) peaks and an essentially unchanged monomer s (Channell 2018).
   Spurious species and aggregate bias in c(s) are a plausible consequence of
   the non-ideal boundary shape, but were not computed here.
3. **The effect weakens as concentration rises,** falling as c0^-0.24 between
   0.001 and 0.1 mg/mL. That is non-monotonic overall, since it is zero without
   flow. Stronger convection homogenizes the cross-section (lateral rms 1.9e-3
   → 1.5e-4). A naive extrapolation to 1 mg/mL gives ΔD/D ≈ 1.8 % and
   Δs/s ≈ 0.15 % per degree at dt = 5 s. With the dt → 0 correction this
   becomes ΔD/D ≈ 1 % per degree. This is unverified: c0 ≥ 0.1 is under-resolved on
   these grids, and the wall-layer Péclet number grows with c0.
4. **The PNK closure is rejected.** The plateau is depleted by about 1 %:
   solute goes to the bottom in the enriched layer, while the depleted layer
   mixes laterally instead of rising to the boundary. The advective closure
   (κ_a, κ_d) barely reduces the misfit (6.2 → 5.8e-3), and its fitted
   parameters trade off against s. The 1D closure in
   `US_CellMisalignment`/`US_LammAstfvm` (PNK, κ) is therefore not supported
   as a description of the hydrodynamics. A dispersive closure is needed, e.g.
   D_eff = D (1 + γ(ψ, c0, ω)) plus a weak plateau sink, calibrated on 2D runs.

## 6. Open points

1. **Accuracy of the 2D results.** The grid sensitivity of the effect is
   about 15 %. The time-step error is first order: ΔD at dt = 5 s is about
   1.7× its dt → 0 value. The numbers are semi-quantitative. Production
   runs need a second-order time integrator (BDF2), wall-clustered grids and
   higher c0 (0.5–1 mg/mL).
2. **Sector geometry.** Run the 2D model in true (r, φ) sector geometry, with
   salt stratification, temperature and multiple species. Then reconstruct
   c(s) to compare with the published aggregate bias.
3. **Replace the C++ closure.** Replace the PNK closure in the C++ prototype
   with the calibrated dispersive closure, keeping the geometry part
   (`US_CellMisalignment`), which is exact.
4. **Out of scope:** tilt of the cell axis, band-forming cells, ASTFEM, GUI
   controls (us_astfem_sim), and us_mpi_analysis.
