"""
Weak-form moment extraction in apparent-sedimentation (s*) space.

Notation (all derivations in PHYSICS.md):

    u(r,t) = ln(r/rm) / (w^2 t)                 apparent s, in seconds
    a(r,t)                                      the raw scan
    g*(s*,t)                                    apparent distribution

For an ideal non-diffusing mixture the scan is the cumulative, dilution-scaled
distribution,

    a(r,t) = \int_0^{u(r,t)} c(s) e^{-2 s w^2 t} ds ,

so that g*(u,t) = (da/du) e^{+2 u w^2 t} = (da/du) (r/rm)^2 recovers c(u).
The moments we want are

    mu_m(t) = \int u^m g*(u,t) du = \int_{rm}^{rb} u^m (r/rm)^2 da/dr dr .

We never evaluate da/dr.  Introducing a window W(r) with W = W' = 0 at the
ends of its support and integrating by parts,

    mu_m^W(t) = \int W Phi_m da/dr dr = - \int a(r,t) d/dr[ W Phi_m ] dr ,
    Phi_m(r,t) = u^m (r/rm)^2 ,

which is a weighted integral of the *raw scan* against an analytically known
kernel.  Expanding the derivative,

    d/dr[ W Phi_m ] = W'(r) u^m (r/rm)^2
                    + W(r) (r/rm)^2 (1/r) [ m u^{m-1}/(w^2 t) + 2 u^m ] .

Two consequences that matter more than the algebra (see FINDINGS.md, D.1):

  * The kernel integrates to zero, \int d/dr[W Phi_m] dr = 0, because W Phi_m
    vanishes at both ends.  Radially-invariant (RI) noise, which is constant
    in r for a given scan, is therefore annihilated *exactly*.

  * Subtracting the time-mean of the scans before extraction annihilates
    time-invariant (TI) noise exactly, because TI noise is by definition
    constant in t.  The same projector is applied inside the forward map, so
    the estimand is unchanged.  See `extract_moments(ti_project=True)`.

Units: u is returned in Svedbergs (1e-13 s) so the numbers are O(1).
"""

import numpy as np

SVEDBERG = 1e-13


# --------------------------------------------------------------------------
# Window / test function
# --------------------------------------------------------------------------

def _smoothstep5(x):
    """Quintic smoothstep: 0 at x<=0, 1 at x>=1, with S' = S'' = 0 at both."""
    x = np.clip(x, 0.0, 1.0)
    return x * x * x * (10.0 + x * (-15.0 + 6.0 * x))


def _smoothstep5_d(x):
    """Derivative of _smoothstep5 (zero outside [0,1])."""
    inside = (x > 0.0) & (x < 1.0)
    xc = np.clip(x, 0.0, 1.0)
    return np.where(inside, 30.0 * xc * xc * (1.0 - xc) ** 2, 0.0)


class Window:
    """
    C^2 plateau bump on [r_lo, r_hi], rising over `taper` at each end.

    W = 0 outside [r_lo, r_hi], = 1 on [r_lo+taper, r_hi-taper].
    W = W' = W'' = 0 at r_lo and r_hi, as required to drop the boundary terms
    in the integration by parts.
    """

    def __init__(self, r_lo, r_hi, taper):
        if r_hi - r_lo <= 2 * taper:
            raise ValueError("window too narrow for the requested taper")
        self.r_lo, self.r_hi, self.taper = float(r_lo), float(r_hi), float(taper)

    def __call__(self, r):
        up = _smoothstep5((r - self.r_lo) / self.taper)
        dn = _smoothstep5((self.r_hi - r) / self.taper)
        return up * dn

    def deriv(self, r):
        up = _smoothstep5((r - self.r_lo) / self.taper)
        dn = _smoothstep5((self.r_hi - r) / self.taper)
        dup = _smoothstep5_d((r - self.r_lo) / self.taper) / self.taper
        ddn = -_smoothstep5_d((self.r_hi - r) / self.taper) / self.taper
        return dup * dn + up * ddn

    def plateau(self):
        return self.r_lo + self.taper, self.r_hi - self.taper


# --------------------------------------------------------------------------
# Kernels
# --------------------------------------------------------------------------

def _omega2(rpm):
    return (rpm * 2.0 * np.pi / 60.0) ** 2


def moment_kernel(r, t, rpm, m, window, rm):
    """
    Weight K_m(r,t) such that  mu_m^W(t) = \int a(r,t) K_m(r,t) dr.

    K_m = -d/dr[ W Phi_m ]
        = -[ W'(r) u^m (r/rm)^2
             + W(r) (r/rm)^2 (1/r) ( m u^{m-1}/(w^2 t) + 2 u^m ) ]

    with u in Svedbergs.
    """
    r = np.asarray(r, dtype=float)
    w2 = _omega2(rpm)
    u = np.log(r / rm) / (w2 * t) / SVEDBERG          # Svedbergs
    dil = (r / rm) ** 2
    W = window(r)
    Wp = window.deriv(r)

    um = u ** m
    if m == 0:
        um1 = np.zeros_like(u)
    else:
        um1 = u ** (m - 1)

    # d(u)/dr in Svedbergs per cm
    dudr = 1.0 / (r * w2 * t) / SVEDBERG

    dPhi = dil * (m * um1 * dudr + 2.0 * um / r)
    return -(Wp * um * dil + W * dPhi)


def s_star(r, t, rpm, rm):
    """Apparent sedimentation coefficient in Svedbergs."""
    return np.log(np.asarray(r) / rm) / (_omega2(rpm) * t) / SVEDBERG


# --------------------------------------------------------------------------
# Extraction
# --------------------------------------------------------------------------

def _simpson_weights(r):
    """Composite-Simpson weights on a uniform grid (falls back to trapezoid)."""
    n = r.size
    h = r[1] - r[0]
    if n % 2 == 0:            # need an odd number of points for pure Simpson
        w = np.empty(n)
        w[:] = h
        w[0] = w[-1] = 0.5 * h
        return w
    w = np.ones(n)
    w[1:-1:2] = 4.0
    w[2:-1:2] = 2.0
    return w * h / 3.0


def extract_moments(scans, r, times, rpm, rm, window, m_max, ti_project=False):
    """
    Weak-form moments for every scan.

    scans : (n_t, n_r) raw readings
    r     : (n_r,) radial grid (uniform)
    times : (n_t,) scan times [s]
    window: Window instance, support strictly inside (rm, r_bottom)

    Returns (n_t, m_max+1) array of mu_m^W(t), u in Svedbergs.

    If ti_project, the time-mean over the supplied scans is removed first,
    which annihilates time-invariant noise exactly.  The same projection must
    be applied to the model prediction (see forward_map.forward_moments).
    """
    scans = np.asarray(scans, dtype=float)
    a = scans - scans.mean(axis=0, keepdims=True) if ti_project else scans
    quad = _simpson_weights(np.asarray(r, dtype=float))

    out = np.empty((len(times), m_max + 1))
    for i, t in enumerate(times):
        for m in range(m_max + 1):
            K = moment_kernel(r, t, rpm, m, window, rm)
            out[i, m] = np.dot(a[i], K * quad)
    return out


def extract_moments_naive(scans, r, times, rpm, rm, window, m_max):
    """
    Reference implementation: differentiate the scan, then integrate.

    mu_m = \int W u^m (r/rm)^2 da/dr dr, with da/dr by central differences.
    Used only for the Gate G0 cross-check against the by-parts form; it is
    unusable on noisy data, which is the whole point of the weak form.
    """
    scans = np.asarray(scans, dtype=float)
    r = np.asarray(r, dtype=float)
    quad = _simpson_weights(r)
    out = np.empty((len(times), m_max + 1))
    for i, t in enumerate(times):
        dadr = np.gradient(scans[i], r, edge_order=2)
        u = s_star(r, t, rpm, rm)
        dil = (r / rm) ** 2
        W = window(r)
        for m in range(m_max + 1):
            out[i, m] = np.dot(W * (u ** m) * dil * dadr, quad)
    return out


# --------------------------------------------------------------------------
# Hermite (Gaussian) deconvolution
# --------------------------------------------------------------------------

def _double_factorial_odd(j):
    """(2j-1)!! with (−1)!! = 1."""
    out = 1.0
    for k in range(1, j + 1):
        out *= (2 * k - 1)
    return out


def hermite_deconvolve(mu, sigma):
    """
    Undo a Gaussian blur of width `sigma` at the level of moments.

        y_m = sum_j C(m,2j) (2j-1)!! (-sigma^2)^j mu_{m-2j}

    This is the exact inverse of moment-space Gaussian convolution: it is the
    forward relation with sigma^2 -> -sigma^2.  The alternating signs are the
    source of the noise amplification measured in Phase D.

    mu    : (..., M+1) moment array
    sigma : scalar, same units as the moment variable (Svedbergs here)
    """
    from math import comb

    mu = np.asarray(mu, dtype=float)
    M = mu.shape[-1] - 1
    y = np.zeros_like(mu)
    s2 = sigma ** 2
    for m in range(M + 1):
        acc = np.zeros(mu.shape[:-1])
        for j in range(m // 2 + 1):
            acc = acc + comb(m, 2 * j) * _double_factorial_odd(j) * ((-s2) ** j) * mu[..., m - 2 * j]
        y[..., m] = acc
    return y


def hermite_convolve(y, sigma):
    """Forward Gaussian blur on moments; inverse of hermite_deconvolve."""
    from math import comb

    y = np.asarray(y, dtype=float)
    M = y.shape[-1] - 1
    mu = np.zeros_like(y)
    s2 = sigma ** 2
    for m in range(M + 1):
        acc = np.zeros(y.shape[:-1])
        for j in range(m // 2 + 1):
            acc = acc + comb(m, 2 * j) * _double_factorial_odd(j) * (s2 ** j) * y[..., m - 2 * j]
        mu[..., m] = acc
    return mu


def central_moments(mu):
    """Mean and variance of the apparent distribution from raw moments."""
    mu = np.asarray(mu, dtype=float)
    mean = mu[..., 1] / mu[..., 0]
    var = mu[..., 2] / mu[..., 0] - mean ** 2
    return mean, var
