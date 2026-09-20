"""
Band-forming (zonal) sedimentation.

Physically a different experiment from sedimentation velocity, not a
parameter change: a thin lamella of sample is layered on a dense buffer and
migrates as a *band*, so the scan is a peak with zero baseline on both sides
rather than a boundary with a plateau.

That changes the moment extraction completely, and mostly for the better.

1. NO DERIVATIVE IS NEEDED.  For a non-diffusing band, substituting
   r0 = r e^{-s w^2 t} into

       a(r,t) = sum_i c_i phi(r e^{-s_i w^2 t}) e^{-2 s_i w^2 t}

   makes the square-dilution factor cancel exactly against the Jacobian, and

       mu_m(t) = \\int u^m (r/rm) a(r,t) dr
               = sum_i c_i \\int (s_i + eps(r0))^m dnu(r0)                (1)

   with dnu(r0) = (r0/rm) phi(r0) dr0 and eps(r0) = ln(r0/rm)/(w^2 t).

   So the moments are a *direct* weighted integral of the raw scan.  The
   integration by parts that SV needs (to avoid differentiating noisy data)
   is simply not required here.

2. THE LAMELLA BLUR IS KNOWN A PRIORI.  Equation (1) says the true atoms are
   convolved with the pushforward of the lamella under eps -- a distribution
   fixed by the band volume and cell geometry, both known before the
   experiment.  It can therefore be deconvolved EXACTLY, by the binomial
   recursion in `deconvolve_known`, instead of being approximated by a
   Gaussian.  Only the diffusion part needs the Hermite treatment.

3. THE BASELINE IS ZERO AWAY FROM THE BAND.  This is the one that matters
   most.  In SV, time-invariant noise cannot be separated from signal without
   a model fit to the full 2-D data (FINDINGS.md D.1), which is the structural
   blocker.  In band mode each radius is band-free for most of the run, so
   TI noise can be estimated directly from the scans in which the band is
   elsewhere -- no model, no fit.  See `estimate_ti_band_gated`.

Against that, one property is LOST: the SV weak-form kernel integrates to
zero and so annihilates radially-invariant noise exactly.  The band kernel
W u^m (r/rm) does not.  RI must instead be estimated from the band-free
radii within each scan, which is what `estimate_ri_band_gated` does.
"""

import numpy as np

SVEDBERG = 1e-13


# --------------------------------------------------------------------------
# The lamella
# --------------------------------------------------------------------------

def lamella_width(meniscus, band_volume=0.015, cp_angle=2.5, cp_pathlen=1.2):
    """
    Width of the layered lamella, exactly as US_Astfem_RSA computes it
    (utils/us_astfem_rsa.cpp:1483):

        base = rm^2 + V * 360 / (angle * pathlen * pi)
        w    = sqrt(base) - rm
    """
    base = meniscus ** 2 + band_volume * 360.0 / (cp_angle * cp_pathlen * np.pi)
    return np.sqrt(base) - meniscus


def lamella_profile(r, meniscus, band_volume=0.015, cp_angle=2.5, cp_pathlen=1.2):
    """
    Initial concentration shape, normalised to a peak of 1:

        phi(r) = exp( -((r - rm)/w)^4 )

    A super-Gaussian, again matching US_Astfem_RSA exactly.  Returned zero
    below the meniscus.
    """
    w = lamella_width(meniscus, band_volume, cp_angle, cp_pathlen)
    x = (np.asarray(r, float) - meniscus) / w
    return np.where(x >= 0.0, np.exp(-np.clip(x, 0, 50) ** 4), 0.0)


def lamella_mass(r, meniscus, **kw):
    """
    Phi' = \\int (r0/rm) phi(r0) dr0, the total weight the moment estimator
    assigns to a species loaded at unit peak concentration.

    This is the conversion between 'signal concentration' (the peak of the
    lamella, which is what US_Model carries) and the mass weight that appears
    in the moment vector.  It is a geometric constant, identical for every
    species, so it rescales y_0 and leaves the atom locations untouched.
    """
    phi = lamella_profile(r, meniscus, **kw)
    return np.trapezoid(phi * (np.asarray(r, float) / meniscus), r)


def lamella_eps_moments(r, meniscus, t, rpm, n_moments, **kw):
    """
    Normalised moments e_j(t) = \\int eps^j dnu / \\int dnu of the lamella's
    footprint in s*-space, with eps = ln(r0/rm)/(w^2 t) in Svedbergs.

    These are the *exact* moments of the blur that the finite lamella width
    imposes -- no Gaussian assumption anywhere.  They shrink like 1/t, so the
    lamella contribution fades as the band migrates.
    """
    r = np.asarray(r, float)
    w2 = (rpm * 2.0 * np.pi / 60.0) ** 2
    phi = lamella_profile(r, meniscus, **kw)
    weight = phi * (r / meniscus)
    with np.errstate(divide="ignore", invalid="ignore"):
        eps = np.log(np.maximum(r, 1e-12) / meniscus) / (w2 * t) / SVEDBERG
    eps = np.where(np.isfinite(eps), eps, 0.0)
    norm = np.trapezoid(weight, r)
    return np.array([np.trapezoid(weight * eps ** j, r) / norm
                     for j in range(n_moments)])


# --------------------------------------------------------------------------
# Moment extraction and deconvolution
# --------------------------------------------------------------------------

def band_moment_kernel(r, t, rpm, m, window, rm):
    """
    Weight K_m(r,t) with mu_m^W(t) = \\int a(r,t) K_m(r,t) dr.

        K_m = W(r) u^m (r/rm),   u in Svedbergs

    Note what is absent: no derivative of the data, and no integration by
    parts.  Compare moments.moment_kernel for the SV case.
    """
    r = np.asarray(r, float)
    w2 = (rpm * 2.0 * np.pi / 60.0) ** 2
    u = np.log(r / rm) / (w2 * t) / SVEDBERG
    return window(r) * (u ** m) * (r / rm)


def extract_moments_band(scans, r, times, rpm, rm, window, m_max):
    """Band-mode moments for every supplied scan, u in Svedbergs."""
    from moments import _simpson_weights

    scans = np.asarray(scans, float)
    r = np.asarray(r, float)
    quad = _simpson_weights(r)
    out = np.empty((len(times), m_max + 1))
    for i, t in enumerate(times):
        for m in range(m_max + 1):
            out[i, m] = np.dot(scans[i], band_moment_kernel(r, t, rpm, m, window, rm) * quad)
    return out


def deconvolve_known(mu, e):
    """
    Exactly undo convolution with a measure whose normalised moments are `e`.

    If  mu_m = sum_j C(m,j) e_j y_{m-j}  with e_0 = 1, then

        y_m = mu_m - sum_{j=1..m} C(m,j) e_j y_{m-j}

    by forward substitution.  Exact for any blur, not just Gaussian, which is
    why the lamella footprint does not have to be approximated.
    """
    from math import comb

    mu = np.asarray(mu, float)
    e = np.asarray(e, float)
    M = mu.shape[-1] - 1
    y = np.zeros_like(mu)
    for m in range(M + 1):
        acc = mu[..., m].copy()
        for j in range(1, m + 1):
            acc = acc - comb(m, j) * e[j] * y[..., m - j]
        y[..., m] = acc
    return y


def blur_sigma_band(t, s, D, rpm, rm, sigma_r0):
    """
    Diffusion blur in s*-space for a band, in Svedbergs.

        sigma^2(t) = 2 D t / (w^4 t^2 r_b^2)

    i.e. the same form as SV.  The lamella's own contribution is NOT folded
    in here: it is removed exactly by deconvolve_known, so only the diffusion
    part is left for the Hermite step.  `sigma_r0` is accepted so callers can
    form the combined width when they want the total observed blur (for
    window sizing).
    """
    w2 = (rpm * 2.0 * np.pi / 60.0) ** 2
    rb = rm * np.exp(s * w2 * t)
    return np.sqrt(2.0 * D * t) / (rb * w2 * t) / SVEDBERG


def blur_sigma_band_total(t, s, D, rpm, rm, sigma_r0):
    """
    Total observed band width in s*-space: lamella and diffusion in
    quadrature.  Used for window placement, where what matters is how wide
    the *observed* peak is, not how much of it is deconvolvable.

        sigma_total^2 = (sigma_r0^2 + 2 D t) / (w^4 t^2 r_b^2)

    At early times the lamella term dominates and the apparent exponent
    approaches -2; at late times diffusion dominates and it approaches the
    SV value near -1.  A single power law does not describe band mode.
    """
    w2 = (rpm * 2.0 * np.pi / 60.0) ** 2
    rb = rm * np.exp(s * w2 * t)
    return np.sqrt(sigma_r0 ** 2 + 2.0 * D * t) / (rb * w2 * t) / SVEDBERG


# --------------------------------------------------------------------------
# Band-gated systematic-noise removal
# --------------------------------------------------------------------------
#
# This is the practical payoff of band forming, and the reason it is worth
# re-running the whole study for.
#
# In SV, every radius outside the boundary carries plateau signal for the
# whole run, so time-invariant noise cannot be separated from the signal
# without fitting a model to all n_t x n_r points -- which reintroduces the
# inverse problem the moment route was meant to avoid (FINDINGS.md D.1).
# Subtracting the time-mean annihilates TI but removes signal with it, biasing
# the estimate by 60% at m=0.
#
# In band mode the signal is a migrating peak with zero baseline, so each
# radius is band-free for most of the run.  TI can be read off directly from
# the scans in which the band is somewhere else, and RI from the radii the
# band is not at.  Neither step needs a model of the distribution -- only the
# band's approximate trajectory, which follows from the a priori s range.

def band_free_mask(r, times, rpm, rm, s_lo, s_hi, D_max, geom, k_gate=6.0):
    """
    Boolean (n_t, n_r) mask, True where the band is NOT present.

    A point is band-free if its apparent s lies more than k_gate blur widths
    outside the a priori support [s_lo, s_hi].  Working in s* rather than in
    radius is what makes one gate width valid for every scan.

    Two exclusions matter, and omitting them silently wrecks the estimate:

      * Once the band reaches the cell bottom the material pellets there and
        stays.  That region is then permanently occupied, not band-free, and
        because it stays occupied for every later scan a median cannot reject
        it.  Scans from after the band has arrived at the bottom are dropped
        entirely.
      * A margin at the bottom is excluded outright, since the pellet has
        finite width.

    Without these the mask happily labels the pellet -- the largest signal in
    the whole run -- as background.
    """
    r = np.asarray(r, float)
    times = np.asarray(times, float)
    w2 = (rpm * 2.0 * np.pi / 60.0) ** 2
    s_ref = 0.5 * (s_lo + s_hi) * SVEDBERG

    bottom = geom.bottom
    sigma_r0 = 0.3145 * lamella_width(rm, band_volume=geom.band_volume)

    mask = np.zeros((times.size, r.size), dtype=bool)
    for i, t in enumerate(times):
        sig = blur_sigma_band_total(t, s_ref, D_max, rpm, rm, sigma_r0)
        # has the leading edge of the band reached the bottom?
        r_lead = rm * np.exp((s_hi * SVEDBERG + k_gate * sig * SVEDBERG) * w2 * t)
        if r_lead >= bottom - 0.05:
            continue                      # pelleting has begun; drop the scan
        u = np.log(r / rm) / (w2 * t) / SVEDBERG
        band_free = (u < s_lo - k_gate * sig) | (u > s_hi + k_gate * sig)
        mask[i] = band_free & (r < bottom - 0.05)
    return mask


def estimate_ti_ri(scans, mask, n_iter=3, min_samples=5):
    """
    Alternating median estimates of TI psi(r) and RI beta(t), using only
    band-free points.

    Medians rather than means, so that a few points where the gate is
    imperfect cannot drag the estimate.  Returns (psi, beta); radii or scans
    with too few band-free samples fall back to zero, which is the right
    default because they are precisely the ones the window covers.
    """
    a = np.asarray(scans, float)
    psi = np.zeros(a.shape[1])
    beta = np.zeros(a.shape[0])

    for _ in range(n_iter):
        resid = a - beta[:, None]
        for j in range(a.shape[1]):
            sel = mask[:, j]
            if sel.sum() >= min_samples:
                psi[j] = np.median(resid[sel, j])
        resid = a - psi[None, :]
        for i in range(a.shape[0]):
            sel = mask[i]
            if sel.sum() >= min_samples:
                beta[i] = np.median(resid[i, sel])
    return psi, beta


def denoise(scans, geom, model, k_gate=6.0, n_iter=3):
    """
    Remove systematic noise from a band-forming run.

    Unlike the SV 'oracle' mode, this is achievable: it uses only the a
    priori support bounds and an upper bound on D, both of which an
    experimenter has before running the sample.
    """
    from models import support_bounds

    s_lo, s_hi = support_bounds(model)
    D_max = max(c["D"] for c in model["components"])
    mask = band_free_mask(geom.r, geom.times, geom.rpm, geom.meniscus,
                          s_lo, s_hi, D_max, geom, k_gate=k_gate)
    psi, beta = estimate_ti_ri(scans, mask, n_iter=n_iter)
    return np.asarray(scans, float) - psi[None, :] - beta[:, None], psi, beta
