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

## 5. Interpretation and open points

1. **The data bound κ.** With κ = 1 the closure predicts an s overestimate of
   about 6 % per degree. That is incompatible with the reported unchanged
   monomer s at ±4° (Channell 2018). The data therefore imply κ ≲ 0.05.
   Wall-layer convection in real cells is much weaker than the PNK limit,
   presumably because of background density stratification (buffer salts,
   temperature), which holds intrusions near their source.
2. **Broadening is not explained.** The reported broadening, and the higher
   aggregate fraction in c(s), are not reproduced by the advective closure.
   A dispersive mechanism is needed. Candidates:
   - Phillips–Wunsch boundary flows at the tilted walls inside the stratified
     boundary region;
   - intrusions spreading at their neutral level.

   Deriving that closure needs an implicit 2D (r, φ) Stokes–transport solver.
   That is the next step, and it would also determine κ from first principles
   as a function of δ, c, ω and the stratification.
3. **Out of scope:** tilt of the cell axis, misalignment in band-forming
   cells, the ASTFEM solver, GUI controls (us_astfem_sim), and the
   us_mpi_analysis job configuration.
