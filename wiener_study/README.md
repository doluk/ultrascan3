# Wiener skewing: effect on UltraScan 2DSA fits

Paper cases (Chaturvedi & Schuck, Chem. Methods 2024) simulated ideally (no concentration dependence) with and without Wiener skewing, then fitted with an UltraScan 2DSA (s × f/f0 grid, subgrid NNLS, TI+RI noise removed algebraically).

## Method
- Simulation: `US_Astfem_RSA` (simpoints 1000; truth on 0.0005 cm grid); 60 scans; Rayleigh data in fringes.
- Skew: object point r imaged at ρ = r + κ′·∂c/∂r (κ′ from paper Table 1: 12 mm 1.51e-5, 1 mm 2.69e-5 cm²/fringe). The skew is applied to the macromolecular signal plus the TI noise (object plane); RI and random noise are added afterwards.
- Fit: grid of 113 s values × 31 f/f0 values (1–4), basis simpoints 200, 32 interleaved subgrids, 2 refinement iterations, final NNLS. The noise projection is identical to `US_SolveSim::nnls_noise`, with the NNLS iteration cap raised to 50·n.
- Noise: none; TI+RI only; random noise 0.005 / 0.01 / 0.02 fringe + TI+RI (3 seeds each).
- Two scan windows: t₀ = 300 s (main, `summary.md`) and t₀ = 1500 s (`t0_1500/summary.md`).

## Results (0.01 fringe noise; RMSD in fringes; t₀ = 300 s / t₀ = 1500 s)

| case | max Δr (µm) | RMSD no skew | RMSD skew | main species, skew | minor species, skew |
|---|---|---|---|---|---|
| IgG 10 mg/ml, 12 mm | 111 / 47 | 0.010 | 0.234 / 0.054 | s ±0.01 S; f/f0 1.60→1.62/1.78 (MW +4 %/+27 %) | dimer f/f0 1.76→3.1–3.5, MW ×2.4–2.7; trimer at f/f0 grid limit |
| IgG 46 mg/ml, 1 mm | 76 / 32 | 0.010 | 0.057 / 0.017 | f/f0 1.60→1.64/1.74 (MW +6 %/+20 %) | dimer f/f0 →2.8–3.2 |
| IgG 50 mg/ml, 1 mm | 86 / 43 | 0.010 | 0.085 / 0.034 | s +0.11/+0.01 S; MW 151→180/184 kDa | – |
| BSA 140 mg/ml, 1 mm | 152 / 96 | 0.010 | 0.278 / 0.096 | f/f0 1.38→1.39/1.49 (MW +3 %/+19 %) | dimer f/f0 1.44→2.7–3.2, MW ×2.6–3.4; trimer fraction 0.03→0.05–0.065 |

## Conclusions
1. Even without concentration dependence, the skew misfit is systematic and lies 2–28× above the random noise level. It is confined to the boundary (diagonal residual stripes) and is largest in early scans, where the boundary is steep and near the meniscus.
2. TI noise fitting does not absorb it: TI noise in the observed image is displaced in time, so it is no longer time-invariant.
3. s values and signal fractions are robust (Δs ≤ 0.1 S, fractions ±0.01–0.03). Skew broadens and shifts the boundary, so the 2DSA misreads it as altered diffusion: f/f0, and hence MW, of the main species is overestimated by 3–27 %. Minor species collapse to the f/f0 upper limit, with MW wrong by ×2–3.
4. For realistic high-concentration data the effect will be larger. Nonideality sharpens boundaries (self-sharpening) and so increases ∂c/∂r.
5. Practical mitigation in UltraScan: exclude early scans, use short pathlengths, and interpret f/f0 and MW from concentrated IP data with caution. The proper fix is to model the skew in the fit; s distributions are less affected.

## Files
- `src/`: C++ driver (links UltraScan `utils`), CMake file, analysis script.
- `*.png`: data, skew difference, residual maps and s distributions per case; `*_2d.png`: s–f/f0 solutes.
- `summary.md`: full tables (all noise levels, mean ± SD over seeds).

## Correction prototype C1 (fixed-point forward model, κ′ known) — `correction_C1/`

**Method:**
- The basis is simulated on an extended radial range (meniscus+0.002 to x₁+0.05 cm), so that skewed points near the edges have source data.
- Each iteration builds the skew map from the current fitted total signal: Δr = κ′·∂c_fit/∂r, forced to be monotonic.
- Every basis column is re-interpolated with that map; no re-simulation is needed.
- The 2DSA is then refit, with TI/RI noise still removed by mean subtraction. Iteration 0 is the uncorrected fit.

**Convergence (noise-free RMSD, fringes):** IgG 10 mg/ml, 12 mm: 0.261 → 0.026 → 0.0097 → 0.0048 → 0.0046; BSA 140 mg/ml: 0.308 → 0.110 → 0.033 → 0.028. Converged after 3–4 iterations; the maximum fitted displacement matches the truth to within about 1 µm.

**Results at 0.01 fringe noise** (RMSD; main-species MW; dimer f/f0 and MW):

| case | no skew | skew, uncorrected | skew, C1 corrected |
|---|---|---|---|
| IgG 10 mg/ml, 12 mm | 0.0101; 151; 1.76/307 | 0.234; 157; 3.46/834 | 0.0111; 151; 1.75/303 |
| IgG 46 mg/ml, 1 mm | 0.0099; 151; 1.76/308 | 0.057; 160; 3.24/758 | 0.0102; 151; 1.76/306 |
| IgG 50 mg/ml, 1 mm | 0.0100; 151 | 0.085; 180 | 0.0104; 151 |
| BSA 140 mg/ml, 1 mm | 0.0104; 67; 1.44/135 | 0.278; 69; 2.72/354 | 0.0296; 66; 1.42/133 |

- Fractions, s, f/f0 and MW of all species are recovered to the accuracy of the no-skew fit. Example: the BSA trimer fraction goes from 0.065 uncorrected back to 0.029 (true value 0.03).
- With noise, the corrected RMSD is within 2–10 % of the noise level for the IgG cases.
- The BSA remainder of 0.028 fringe sits only in scan 1 at r < 5.95 cm, where the map folds over at the meniscus. Excluding that region gives 0.0027 fringe, equal to the no-skew fit.
- Mean-subtracting TI noise (which is skewed in the data) adds only about 0.001 fringe; an explicit fit of skewed TI noise is not needed here.
- Remaining limitations: κ′ was known exactly, skew was simulated without concentration dependence, and the map's fold-over region is not modelled (exclude it, or the first scan, instead).

## Paper data (Chaturvedi & Schuck 2024, Harvard Dataverse) — `paper_data/`

**Check of the skew operator against SEDFIT.** We applied our skew operator (κ′ = 2.69e-5 cm²/fringe) to SEDFIT's unskewed Fig. 3 simulation (`hinderedsingle`) and compared the result with SEDFIT's skewed version (`skewed`). They agree to 0.0029 fringe RMS, against a total skew effect of 0.47 fringe RMS. The largest local difference is 0.18 fringe, in scan 1 at the meniscus. So the two implementations use the same skew model.

**Real data, UltraScan-type ideal 2DSA, without and with C1 correction (κ′ from paper Table 1).**
- Settings: meniscus and fit range from the SEDFIT fit files; TI+RI noise fitted; data interpolated onto a 0.0007 cm grid.
- Scans used: all scans for IgG; every 10th scan for BSA 140 mg/ml; every 5th scan for the BSA control.
- Timing: the rotor acceleration was estimated from scan 1, and the resulting ω²t matches the file headers to within 0.1 %.
- BSA control meniscus: fitted (6.077 cm).

| data set | fit | RMSD (fringe) | monomer frac / s / f/f0 / MW | dimer frac / s / f/f0 / MW | paper |
|---|---|---|---|---|---|
| hIgG 10 mg/ml, 12 mm | 2DSA | 0.0577 | 0.785 / 6.07 / 1.93 / 199 | 0.132 / 9.09 / 3.81 / 948 | c_HS(s₀): 6.44 S 74.1 %; 9.3 S 15.8 %; SEDFIT rmsd 0.041 (ScanRms.dat) |
| | 2DSA + C1 | 0.0391 | 0.759 / 6.10 / 1.88 / 183 | 0.141 / 8.98 / 2.24 / 468 | |
| hIgG 46 mg/ml, 1 mm | 2DSA | 0.0978 | main peak at 4.8 S (apparent) | – | 6.73 S 71.1 %; 9.4 S 16.5 % |
| | 2DSA + C1 | 0.1133 | – | – | |
| BSA 140 mg/ml, 1 mm | 2DSA | 1.85 (1.88 with bottom 6.60) | main peak at 1.3 S (apparent) | – | 3.9 S; dimer 22 %; rmsd 0.175 |
| | 2DSA + C1 | 1.98 | – | – | |
| BSA 0.5 mg/ml (control) | 2DSA | 0.0030 | 0.647 / 4.19 / 1.48 / 73 | 0.236 / 6.34 / 1.65 / 171 | c(s): 4.2 S; dimer 19 %; f/f0 1.43 |

**Interpretation.**
- **10 mg/ml IgG, 12 mm:** the skew correction lowers the RMSD by 32 %, to the level of the paper's SEDFIT fit (0.039 vs 0.041). It also halves the spurious dimer MW (948 → 468 kDa). What remains (monomer 6.1 vs 6.44 S; dimer MW still too high) is consistent with uncorrected nonideality: k_s·c ≈ 0.1 at 10 mg/ml.
- **46 and 140 mg/ml:** the ideal 2DSA cannot describe the self-sharpened boundaries (diagonal residual band; RMSD 0.10 and 1.9 fringe). Applying the skew correction on top of this wrong boundary model makes the RMSD worse. The skew correction can therefore be judged on these data only together with a concentration-dependent sedimentation model (e.g. the paper's hindered settling, c_HS(s₀)), which UltraScan's 2DSA does not have.
- **BSA control:** consistent with the paper (4.19 vs 4.2 S; dimer 24 % vs 19 % within the 5.2–7.6 S window; f/f0 1.48 vs 1.43).

## Stage 1: σ fit with the ASTFVM of branch `cosedimenting` — `stage1_sigma/`

**Setup.**
- The `utils` of branch `cosedimenting` were built into the driver (`src/cosed_driver/`). Each 2DSA basis function is simulated with `US_LammAstfvm` (non-ideal case 1, s = s₀/(1+σc), δ = 0), as `US_SolveSim` does on that branch.
- Basis functions are simulated at unit signal (`signal_concentration = 1`), so σ is effectively dimensionless (≈ σ_phys·c₀). c is each basis function's own normalised concentration, not the total concentration.
- σ was scanned as a grid parameter. At each σ: a 2DSA with 57 s × 13 f/f0 values, TI+RI noise fitted, then C1 skew correction (4 iterations; κ′ from paper Table 1).
- Numerics: ASTFVM at simpoints 200 takes about 0.55 s per basis function (ASTFEM: about 8 ms). With σ = 0 it reproduces the ASTFEM fit to 0.0015 fringe.

**Results** (best σ by RMSD; RMSD in fringes):

| data set | ideal ASTFEM (+C1) | best σ: RMSD no skew / +C1 | monomer frac / s / MW (+C1) | dimer frac / MW (+C1) | paper |
|---|---|---|---|---|---|
| hIgG 10 mg/ml, 12 mm | 0.058 (0.039) | σ = 0: 0.060 / 0.040 | 0.765 / 6.10 S / 181 kDa | 0.136 / 447 kDa | 6.44 S 74.1 %; 9.3 S 15.8 %; rmsd 0.041 |
| hIgG 46 mg/ml, 1 mm | 0.098 (0.113) | σ = 0.2: 0.059 / 0.054 | 0.913 / 5.87 S / 184 kDa | 0.044 / 126 kDa | 6.73 S 71.1 %; 9.4 S 16.5 % |
| BSA 140 mg/ml, 1 mm | 1.88 (2.0) | σ = 0: 2.0; σ = 0.5 with C1: 0.97 | not meaningful | – | 3.9 S; dimer 22 %; rmsd 0.175 |

**Interpretation.**
- **IgG 46 mg/ml:** the self-concentration σ model captures part of the self-sharpening (RMSD 0.110 → 0.059; 0.054 with skew correction). The composition is wrong, though: dimer 4 % instead of 16.5 %, monomer s 5.9 instead of 6.7 S. This is expected from a model in which every basis function is slowed by its own normalised profile instead of the total concentration, so the Johnston–Ogston effect is not described.
- **IgG 10 mg/ml:** σ > 0 does not lower the RMSD; σ = 0 (+C1) remains best. At σ = 0.05 the molar masses become more plausible (monomer 157, dimer 299 kDa) at the same RMSD (0.0398).
- **BSA 140 mg/ml:** the hyperbolic self-concentration model cannot describe the extremely steep boundaries (RMSD ≥ 0.97 vs 0.175 in the paper). The C1 skew map computed from such poor fits is unphysical (displacements above 1 mm). C1 requires a model that already describes the boundary shape.
- **Conclusion:** the σ fit of the branch is not sufficient for concentrated mixtures. Stage 2 (mean-field coupling to c_tot(r,t), and a hindered-settling functional form) is needed.

**Limitations:** coarse grid (57 × 13), simpoints 200, δ fixed at 0 (in the branch both signs of δ decrease D).
