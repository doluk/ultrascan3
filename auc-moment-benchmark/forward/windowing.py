"""
Window placement, and the observable time window (spec B.3).

This module encodes the single hardest constraint in the whole method, which
is easy to miss until the numbers are on the table:

  In s*-space the window acts as  W~(s*,t) = W(rm exp(s* w^2 t)).  The moment
  mu_m^W(t) equals the true windowed moment only where W~ == 1 over the
  support of g*.  But g* is the true distribution *blurred by diffusion* with
  width sigma(t), and at realistic times sigma(t) is LARGER than the a priori
  support interval [s_min, s_max].  The window plateau must therefore cover

        [ s_min - k sigma(t) ,  s_max + k sigma(t) ]

  not merely [s_min, s_max].  Sizing the window to the support alone
  truncates the tails of g* and biases every moment from m = 2 upward -- in
  our first pass it made the measured sigma^2 about 45x too small and
  manufactured a spurious rpm dependence.

Mapped back to radius, the plateau must span roughly r_b(t) +- k sqrt(2 D t),
and that must fit strictly inside (meniscus, bottom).  The lower edge clears
the meniscus only after the boundary has moved k diffusion widths away from
it; the upper edge hits the bottom soon after.  The intersection is the
observable time window, and it is narrow.
"""

import numpy as np

from moments import Window

SV = 1e-13


def _omega2(rpm):
    return (rpm * 2.0 * np.pi / 60.0) ** 2


def blur_sigma(t, s, D, rpm, rm):
    """
    Diffusion blur width in s*-space, in Svedbergs (the vHW-form law, which
    Phase C.1 confirms empirically):

        sigma^2 = 2 D / (w^4 t r_b^2),   r_b = rm e^{s w^2 t}
    """
    w2 = _omega2(rpm)
    rb = rm * np.exp(s * w2 * t)
    return np.sqrt(2.0 * D / (w2 ** 2 * t * rb ** 2)) / SV


def place_window(geom, s_lo, s_hi, t, D_max, s_ref, k_sigma=4.0,
                 taper_frac=0.25, margin=0.03, band=False):
    """
    Window whose plateau covers the support padded by k_sigma blur widths.

    s_lo, s_hi : a priori support bounds [Svedberg]
    D_max      : upper bound on the diffusion coefficients present
    s_ref      : representative s used to locate the boundary
    margin     : required clearance from meniscus and bottom [cm]

    Returns a Window, or None if no admissible window fits in the cell.
    """
    w2 = geom.omega2
    if band:
        sig = band_sigma_total(t, s_ref, D_max, geom.rpm, geom.meniscus, geom)
    else:
        sig = blur_sigma(t, s_ref, D_max, geom.rpm, geom.meniscus)
    lo = (s_lo - k_sigma * sig) * SV
    hi = (s_hi + k_sigma * sig) * SV

    r_lo_p = geom.meniscus * np.exp(lo * w2 * t)
    r_hi_p = geom.meniscus * np.exp(hi * w2 * t)
    width = r_hi_p - r_lo_p
    if width <= 0:
        return None
    taper = max(taper_frac * width, 0.01)
    r_lo, r_hi = r_lo_p - taper, r_hi_p + taper
    if r_lo < geom.meniscus + margin or r_hi > geom.bottom - margin:
        return None
    return Window(r_lo, r_hi, taper)


def band_sigma_total(t, s, D, rpm, rm, geom):
    """Observed band width (lamella and diffusion in quadrature)."""
    from band import blur_sigma_band_total, lamella_width
    import numpy as _np
    # sigma of the exp(-x^4) lamella in r, as a fraction of its width w
    w = lamella_width(rm, band_volume=geom.band_volume)
    sigma_r0 = 0.3145 * w          # sqrt(Var) of exp(-x^4) on x>=0, in units of w
    return blur_sigma_band_total(t, s, D, rpm, rm, sigma_r0)


def observable_window(geom, model, k_sigma=4.0, **kw):
    """
    Scans for which an admissible window exists.

    Returns a list of (scan_index, time, Window).  Its length is the number
    of usable scans -- a first-order constraint on the whole method.
    """
    from models import support_bounds

    s_lo, s_hi = support_bounds(model)
    comps = model["components"]
    D_max = max(c["D"] for c in comps)
    wsum = sum(c["c"] for c in comps) or 1.0
    s_ref = sum(c["c"] * c["s"] for c in comps) / wsum
    if s_ref <= 0:
        s_ref = np.mean([c["s"] for c in comps])

    band = getattr(geom, "band", False)
    out = []
    for i, t in enumerate(geom.times):
        w = place_window(geom, s_lo, s_hi, t, D_max, s_ref, k_sigma=k_sigma,
                         band=band, **kw)
        if w is not None:
            out.append((i, t, w))
    return out
