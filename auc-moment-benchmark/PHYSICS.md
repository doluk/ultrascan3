# Derivations

Everything the pipeline does, written out once. Notation is fixed here and
used unchanged in `forward/`.

| symbol | meaning |
|---|---|
| `a(r,t)` | raw scan: concentration (or fringes / OD) at radius `r`, time `t` |
| `r_m`, `r_b` | meniscus and cell bottom |
| `w` | rotor angular velocity, `rpm * 2*pi/60` |
| `u = s*(r,t)` | apparent sedimentation coefficient, see §1 |
| `c(s)` | the true distribution — the measure we want |
| `g*(u,t)` | apparent distribution at time `t` |
| `y_k` | `\int s^k c(s) ds` — the true moments, the handoff quantity |

Units: `u` is carried in Svedbergs (`1e-13 s`) everywhere, so the numbers in
`y` are O(1)–O(10^m) rather than O(10^-13m).

---

## 1. The `s*` transform and `g*`

A non-diffusing species of coefficient `s` has its boundary at
`r_s(t) = r_m exp(s w^2 t)`, and its plateau is diluted by the square-dilution
law to `c_0 exp(-2 s w^2 t)`. So define

```
u(r,t) = ln(r/r_m) / (w^2 t) .
```

At radius `r` and time `t`, the species still contributing are those whose
boundary has not yet passed `r`, i.e. those with `s < u(r,t)`. Hence

```
a(r,t) = \int_0^{u(r,t)} c(s) e^{-2 s w^2 t} ds .                     (1)
```

Differentiating (1) with respect to `u`,

```
da/du = c(u) e^{-2 u w^2 t} ,
```

and since `u w^2 t = ln(r/r_m)`, the dilution factor is exactly `(r/r_m)^2`.
So the natural definition of the apparent distribution,

```
g*(u,t) = (da/du) (r/r_m)^2 ,                                        (2)
```

satisfies `g* = c` exactly in the ideal non-diffusing limit. The
`(r/r_m)^2` Jacobian *is* the radial-dilution correction; they are the same
factor, not two separate ones.

`us_dcdt` uses the same `u` (see `programs/us_dcdt/us_dcdt.cpp:232`,
`s = s20w_correction * 1e13 * log(radius/meniscus) / (omega^2 * t)`), but
normalises by the plateau rather than applying `(r/r_m)^2`, because it works
from `dc/dt` rather than `dc/dr`. The conventions agree on `u`; they differ
in the amplitude factor, and (2) is the one that makes `g* -> c`.

## 2. Moments as a weighted integral of the raw scan

The moments we want are

```
mu_m(t) = \int u^m g*(u,t) du .
```

Changing variables back to `r` with `du = dr/(r w^2 t)` and `dr/du = r w^2 t`:

```
mu_m(t) = \int_{r_m}^{r_b} u^m (r/r_m)^2 da/dr dr .                   (3)
```

**Do not evaluate (3) as written.** Differentiating a noisy scan amplifies
high-frequency noise, and the whole point is to reach high `m`. Introduce a
test function `W(r)` with `W = W' = 0` at the ends of its support, write
`Phi_m(r,t) = u^m (r/r_m)^2`, and integrate by parts:

```
mu_m^W(t) = \int W Phi_m da/dr dr = -\int a(r,t) d/dr[ W Phi_m ] dr .  (4)
```

The boundary term `[W Phi_m a]` vanishes because `W` does. Expanding, with
`du/dr = 1/(r w^2 t)`:

```
d/dr[W Phi_m] = W'(r) u^m (r/r_m)^2
              + W(r) (r/r_m)^2 (1/r) [ m u^{m-1}/(w^2 t) + 2 u^m ] .   (5)
```

So `mu_m^W(t)` is a weighted integral of the **raw scan** against an
analytically known kernel `K_m = -d/dr[W Phi_m]`. No derivative of the data
is ever taken. This is implemented in `moments.moment_kernel`.

Gate G0 checks (4) against a direct evaluation of (3) on noise-free data.

### 2.1 Two noise properties that fall out of (4)

**RI noise is annihilated exactly.** Radially-invariant noise adds `beta(t)`,
constant in `r`. Its contribution is `beta(t) \int K_m dr`, and

```
\int_{r_lo}^{r_hi} d/dr[W Phi_m] dr = [W Phi_m] = 0
```

because `W` vanishes at both ends. Verified numerically to 5e-10 relative.

**TI noise is not.** Time-invariant noise `psi(r)` contributes
`-\int psi(r) K_m(r,t) dr`, which is non-zero and varies with `t` through the
explicit `t`-dependence of `K_m`. Subtracting the time-mean of the scans
removes `psi` exactly, but it also removes part of the signal, so the
estimand changes; measured bias is 60–3000% (see FINDINGS.md, D.1). Removing
TI without biasing the estimate requires a model fit to the full 2-D data,
which is the inverse problem the moment route was meant to sidestep. This is
the central structural finding of the prototype.

## 3. The window, and why it dominates everything

In `s*`-space the window acts as `W~(u,t) = W(r_m e^{u w^2 t})`, so

```
mu_m^W(t) = \int u^m W~(u,t) g*(u,t) du ,
```

which equals the true moment only where `W~ = 1` over the support of `g*`.
But `g*` is `c` **blurred by diffusion** with width `sigma(t)` (§4), and at
realistic times `sigma(t)` is *larger than the a priori support interval*.

So the plateau of `W` must cover `[s_min - k sigma(t), s_max + k sigma(t)]`,
not `[s_min, s_max]`. Sizing it to the support alone truncates the tails of
`g*` and biases every moment from `m = 2` up: in the first pass here it made
the measured `sigma^2` about 45x too small and manufactured a spurious `rpm`
dependence.

Required `k`, from the relative truncation error of a Gaussian at `+-k sigma`:

| `k` | m=0 | m=4 | m=8 | m=10 |
|---|---|---|---|---|
| 2 | 4.6e-2 | 5.8e-2 | 1.0e-1 | 1.3e-1 |
| 3 | 2.7e-3 | 4.2e-3 | 1.0e-2 | 1.6e-2 |
| **4** | **6.3e-5** | **1.3e-4** | **4.1e-4** | **7.2e-4** |
| 5 | 5.7e-7 | 1.5e-6 | 6.3e-6 | 1.2e-5 |

(`sigma/s0 = 0.1`.) `k = 4` puts truncation safely below the noise floor;
`k = 2` does not. `k = 4` is used throughout.

Mapped back to radius, the plateau must span roughly
`r_b(t) +- k sqrt(2 D t)` and fit strictly inside `(r_m, r_b)`. The lower
edge clears the meniscus only once the boundary has moved `k` diffusion
widths away from it; the upper edge hits the bottom shortly after. The
intersection is the **observable time window** (spec B.3), and it is narrow —
typically 13–21 of 100 scans, and *empty* for small proteins.

`W` is a quintic-smoothstep plateau bump: `W = W' = W'' = 0` at both ends of
its support, `W = 1` across the middle (`moments.Window`).

## 4. The diffusion blur

A sedimenting boundary spreads in `r` as `sigma_r ~ sqrt(2 D t)`. Mapping to
`s*`-space with `du/dr = 1/(r w^2 t)` evaluated at the boundary `r_b`:

```
sigma_u = sigma_r / (r_b w^2 t) = sqrt(2 D t) / (r_b w^2 t)
```

```
=>   sigma^2(t) = 2 D / ( w^4 t r_b(t)^2 ) ,   r_b(t) = r_m e^{s w^2 t} .  (6)
```

This is the `t^(-1/2)` behaviour of `sigma` that van Holde–Weischet
extrapolation relies on. Because `r_b` grows exponentially, the *local*
log-log slope of `sigma^2` is

```
d ln sigma^2 / d ln t = -1 - 2 s w^2 t ,
```

slightly steeper than `-1`.

The proposal instead asserts `sigma^2(t) = 2 D / (w^4 t^3)`. The two differ
by a factor `t^2 / r_b^2`, about five orders of magnitude at realistic times.
Phase C.1 measures the exponent as **-1.073 +- 0.004** across 18
`(s, D, rpm)` combinations, and the prefactor ratio to (6) as **1.09**,
stable to ~2%. Equation (6) is right — up to that 9% excess, which is the
sector-dilution/Faxén correction — and the proposal's `t^-3` is not.

## 5. Hermite deconvolution

If `g* = c * N(0, sigma^2)` then the moments convolve by the binomial
relation with Gaussian moments `M_{2j} = (2j-1)!! sigma^{2j}`:

```
mu_m = sum_j C(m,2j) (2j-1)!! sigma^{2j} y_{m-2j} .                    (7)
```

The inverse is (7) with `sigma^2 -> -sigma^2`:

```
y_m = sum_j C(m,2j) (2j-1)!! (-sigma^2)^j mu_{m-2j} .                  (8)
```

(These are Hermite polynomials in disguise.) Round-trip agreement is 5.7e-14.
The alternating signs in (8) are the source of the noise amplification
measured in Phase D: the relative error of `y_m` grows roughly geometrically
in `m`.

For a mixture each species has its own `D` and therefore its own `sigma`, so
a single `sigma` in (8) is a model error, not just an approximation. Phase
C.2 measures it at **6.2% median / 8.2% max** on a two-species mixture —
larger than the white-noise moment error, and of the same order as the
absorbance-optics error. The proposal's "mean diffusion" assumption is in
direct tension with its own claim that each species carries its own `D`.

## 6. The exact Lamm weak form (independent cross-check)

Multiplying the Lamm equation by `W(r) r` and integrating by parts twice:

```
d/dt \int W c r dr = s w^2 \int W'(r) r^3 c dr + D \int (r W'' + W') c dr .
```

For a **single species** this recovers `(s, D)` by linear least squares with
no Gaussian assumption anywhere, and so validates the `s*`-space pipeline
independently. For mixtures it becomes a linear constraint on the unknown
decomposition rather than a direct estimator — a diagnostic, not the method.
