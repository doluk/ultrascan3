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
