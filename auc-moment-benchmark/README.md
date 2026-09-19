# A truncated Hausdorff moment problem, with a measured covariance

This package states a concrete inverse problem and supplies real data for it.
No background in analytical ultracentrifugation is needed to read this file;
§Provenance says where the numbers come from, and PHYSICS.md has the physics.

## The problem

> Given `y_hat in R^N` with known covariance `Sigma in R^{NxN}`, where
> `y_hat` estimates the moment sequence
>
> ```
>     y_k = \int s^k dmu(s) ,   k = 0 .. N-1
> ```
>
> of an unknown non-negative measure `mu` supported on a **known bounded
> interval** `[s_min, s_max]`, and believed to be atomic with `R <= 4` atoms:
> recover the atoms `(s_i, c_i)`, and certify `R`.

Two features of this instance are worth stating up front, because they are
what make it different from the generic version:

**1. The support interval is known a priori, from the experiment.** This is a
**Hausdorff** moment problem on a bounded interval, not a Hamburger problem on
the line. That is materially better conditioned and the original proposal
never says so.

**2. `Sigma` is nearly rank-one.** The maximum absolute off-diagonal
correlation is **0.997–1.000** across every case shipped here. All moment
orders are estimated from the same ~15 measurements through kernels that
differ only by a smooth power of the integration variable, so their errors are
almost perfectly correlated. Any formulation that assumes isotropic or
independent moment noise is solving a different problem from this one.

## Realistic values

`N = 15` moments are supplied (`m = 0..14`). What is actually usable depends
entirely on the noise assumption, which is the point:

| case | `N_eff` | `R_max` |
|---|---|---|
| white noise only (the assumption the proposal makes) | 11–12 | 5 |
| a realistic favourable instrument | 3–5 | 1–2 |
| a realistic interference instrument | **-1** | — |
| interference with *oracle* systematic-noise removal | 12 | 5 |

`N_eff` is the largest `m` for which the total relative error
`sqrt(Sigma_mm + bias_m^2)/|y_m|` stays below 10%; `R_max = floor((N_eff-1)/2)`.
`N_eff = -1` means even `y_0`, the total mass, is wrong by more than 10%.

Relative moment error at `m = 4`, white-noise case: **~7e-3**.

Conditioning of the noise-free Hankel matrix (leading `R x R` block, moments
rescaled so `s` is O(1)):

| instance | `R` | separation | `cond` |
|---|---|---|---|
| `F-S1` | 1 | — | 1.0 |
| `F-M2-15` | 2 | 15% | 8.2e2 |
| `F-M2-10` | 2 | 10% | 1.8e3 |
| `F-M2-05` | 2 | 5% | 6.7e3 |
| `F-M2-03` | 2 | 3% | 1.8e4 |
| `F-M3` | 3 | 6% | 7.9e6 |

## Layout

```
data/<model_id>__<optics>-<systematic-noise-handling>/
    y_true.npy        ground truth, y_k = sum_i c_i s_i^k          (N,)
    y_hat.npy         mean of the Monte Carlo estimates            (N,)
    Sigma.npy         full covariance of y_hat                     (N,N)
    realizations.npy  all 400 Monte Carlo samples                  (400,N)
    truth.json        atoms (s_i, D_i, c_i), support, R, N_eff,
                      per-order relative error and bias, Hankel
                      conditioning, correlation summary
data/summary.json     every case in one file

forward/forward_map.py   moments_of_measure(s, c, N) -> y, callable, no
                         UltraScan dependency; also the full physical
                         pipeline that produced y_hat
forward/                 the simulator and estimator (numpy/scipy only)
analysis/                the scripts that produced FINDINGS.md
generate/                Layer 1, C++/UltraScan, for reproducibility only
```

`y_hat` is **not** unbiased; `bias = y_hat - y_true` is reported per order in
`truth.json` and is part of the problem, not an artefact to be ignored. For
the systematic-noise cases the bias dominates the variance by orders of
magnitude.

Four noise cases are shipped per model rather than one, because the answer to
"is this solvable?" is different for each, and choosing on your behalf would
hide the finding.

## Please read FINDINGS.md before investing time

The honest summary: of the five go/no-go gates the physics side set itself,
G0 and G1 pass, and **G2, G3 and G4 fail**.

The most important one for a mathematician deciding whether to engage is
**G3**: a conventional non-negative least-squares fit to the raw data
recovers the atoms 5–40x more accurately than inverting these moments does, at
every separation from 15% down to 3% — and that comparison already gives the
moment route advantages the real experiment cannot (favourable optics, exact
removal of the dominant systematic).

**G4** is the one that should worry anyone planning to certify `R`: on a
measure with no atomic decomposition at all, the estimated Hankel spectrum
shows a *confident* rank gap (144x for a smooth unimodal measure, 260x for a
chemically reacting system) — larger than the gap shown by a genuinely rank-2
instance. A rank certificate computed from these moments at this noise level
would be wrong, and would look convincing while being wrong.

None of that makes the truncated moment problem itself uninteresting. It does
mean the physics does not currently deliver moments good enough for it to
matter, and the honest question to ask first is whether any of the noise cases
above is one where recovery is even possible.

## Provenance

`y_hat` and `Sigma` come from 400 Monte Carlo noise realizations pushed
through the complete physical pipeline: simulate the experiment, add noise,
extract moments by a weak-form (integration-by-parts) functional of the raw
signal, deconvolve the diffusion blur, average over the usable measurements.
The simulator is a conservative finite-volume solver validated against a
closed-form approximation and converged in space and time to ~1e-6, four
orders below the noise floor. PHYSICS.md §2–§5 derives the estimator;
FINDINGS.md documents what was validated and what was not.

The measure `mu` is a distribution of sedimentation coefficients; `s` is in
Svedbergs (1e-13 s). `truth.json` carries a second parameter `D_i` per atom
(a diffusion coefficient) which sets the width of the blur that had to be
deconvolved; it is not part of the moment problem, but it is why the estimator
is biased.

## Reproducing

```
pip install numpy scipy
python3 benchmark.py --n-mc 400 --m-max 14     # regenerate data/
python3 analysis/phaseA.py                     # solver validation, gate G0
python3 analysis/phaseC.py                     # blur scaling, gate G1
python3 analysis/phaseD.py                     # covariance, gate G2
python3 analysis/phaseE.py                     # comparators, gates G3/G4
```

Nothing in `forward/`, `analysis/` or `benchmark.py` imports UltraScan or Qt.
