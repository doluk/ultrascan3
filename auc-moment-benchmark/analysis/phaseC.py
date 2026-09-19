"""
Phase C -- the diffusion blur, measured rather than assumed.

The proposal asserts   sigma^2(t) = 2D / (w^4 t^3).
Transforming a radial spread sqrt(2Dt) into s*-space instead gives

    sigma_s* = sigma_r / (r_b w^2 t) = sqrt(2 D t) / (r_b w^2 t)
    =>  sigma^2(t) = 2 D / (w^4 t r_b(t)^2),      r_b(t) = rm e^{s w^2 t}

which is the scaling behind van Holde-Weischet's t^(-1/2) extrapolation.  The
two differ by a factor t^2 / r_b^2 -- about five orders of magnitude at
realistic times -- so WP1's "regression across t^-3" deliverable depends
entirely on which is right.  C.1 settles it empirically.
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "forward"))

import numpy as np
import models, moments as M, lamm
from windowing import observable_window as usable_scans

SV = 1e-13


def sigma2_apparent(model, geom, n_cells=8000, m_max=2, scans=None):
    """Central second moment of g*(s*,t), in Svedberg^2, per usable scan."""
    if scans is None:
        scans = models.simulate(model, geom, n_cells=n_cells)
    u = usable_scans(geom, model)
    ts, s2, mean = [], [], []
    for i, t, w in u:
        mu = M.extract_moments(scans[i:i + 1], geom.r, [t], geom.rpm,
                               geom.meniscus, w, m_max)[0]
        mn, var = M.central_moments(mu)
        ts.append(t); s2.append(var); mean.append(mn)
    return np.array(ts), np.array(s2), np.array(mean)


def law_vhw(t, s, D, rpm, rm):
    """sigma^2 = 2D / (w^4 t r_b^2), the s*-transform of sqrt(2Dt)."""
    w2 = lamm.omega(rpm) ** 2
    rb = rm * np.exp(s * w2 * t)
    return 2.0 * D / (w2 ** 2 * t * rb ** 2) / SV ** 2      # Svedberg^2


def law_proposal(t, s, D, rpm, rm):
    """sigma^2 = 2D / (w^4 t^3), as stated in the proposal."""
    w2 = lamm.omega(rpm) ** 2
    return 2.0 * D / (w2 ** 2 * t ** 3) / SV ** 2


def fit_loglog(t, y):
    """Slope of log y vs log t with a standard-error estimate."""
    x = np.log(t); ly = np.log(y)
    n = x.size
    A = np.vstack([x, np.ones(n)]).T
    coef, res, *_ = np.linalg.lstsq(A, ly, rcond=None)
    pred = A @ coef
    dof = max(n - 2, 1)
    s2 = ((ly - pred) ** 2).sum() / dof
    cov = s2 * np.linalg.inv(A.T @ A)
    return coef[0], np.sqrt(cov[0, 0]), coef[1]


def c1_scaling(verbose=True):
    """Exponent of sigma^2(t) across a grid of (s, D, rpm)."""
    rows = []
    for s_sv in (10.0, 15.0, 22.0):
        for f_f0 in (1.1, 1.25, 1.6):        # varies D at fixed s
            for rpm in (30000, 40000, 50000):
                geom = models.RunGeometry(rpm=rpm, n_scans=120)
                comp = models.species(s_sv, 0.5, f_f0=f_f0)
                model = dict(components=[comp], atomic=True, reaction=None,
                             note="", support=None)
                t, s2, _ = sigma2_apparent(model, geom)
                ok = s2 > 0
                if ok.sum() < 10:
                    continue
                t, s2 = t[ok], s2[ok]
                slope, se, _ = fit_loglog(t, s2)
                pv = law_vhw(t, comp["s"], comp["D"], rpm, geom.meniscus)
                pp = law_proposal(t, comp["s"], comp["D"], rpm, geom.meniscus)
                rows.append(dict(s=s_sv, f_f0=f_f0, rpm=rpm, D=comp["D"],
                                 slope=slope, se=se,
                                 ratio_vhw=float(np.median(s2 / pv)),
                                 ratio_prop=float(np.median(s2 / pp))))
                if verbose:
                    print(f"  s={s_sv:4.1f}S f/f0={f_f0:4.2f} rpm={rpm:5d} "
                          f"D={comp['D']:.2e}  slope={slope:+.3f}+-{se:.3f}  "
                          f"meas/vHW={rows[-1]['ratio_vhw']:.3f}  "
                          f"meas/proposal={rows[-1]['ratio_prop']:.3e}")
    return rows


def c2_species_dependent_blur(verbose=True):
    """
    On M2-10, compare the measured sigma^2_app(t) against the single-sigma
    model implied by a concentration-weighted mean D.  The residual is the
    model error introduced by the proposal's 'mean diffusion' assumption.
    """
    model = models.feasible_test_set()["F-M2-10"]
    geom = models.RunGeometry(rpm=40000, n_scans=120)
    t, s2, mean = sigma2_apparent(model, geom)
    ok = s2 > 0
    t, s2, mean = t[ok], s2[ok], mean[ok]

    comps = model["components"]
    wsum = sum(c["c"] for c in comps)
    Dbar = sum(c["c"] * c["D"] for c in comps) / wsum
    sbar = sum(c["c"] * c["s"] for c in comps) / wsum
    # single-sigma prediction plus the *true* spread of the two atoms, which
    # is part of the second moment and must not be attributed to diffusion
    blur = law_vhw(t, sbar, Dbar, geom.rpm, geom.meniscus)
    atoms = sum(c["c"] * (c["s"] / SV - sbar / SV) ** 2 for c in comps) / wsum
    pred = blur + atoms
    resid = (s2 - pred) / s2
    if verbose:
        print(f"  mean D = {Dbar:.3e}, atom spread = {atoms:.4f} S^2")
        for k in range(0, len(t), max(1, len(t) // 8)):
            print(f"   t={t[k]:7.0f}  meas={s2[k]:.5f}  single-sigma pred={pred[k]:.5f}"
                  f"  rel resid={resid[k]:+.3%}")
        print(f"  median |rel residual| = {np.median(np.abs(resid)):.3%}"
              f"   max = {np.abs(resid).max():.3%}")
    return t, s2, pred, resid


if __name__ == "__main__":
    print("C.1  sigma^2(t) scaling exponent across (s, D, rpm)")
    rows = c1_scaling()
    sl = np.array([r["slope"] for r in rows])
    rv = np.array([r["ratio_vhw"] for r in rows])
    print(f"\n  exponent of sigma^2 vs t : mean {sl.mean():+.3f}  sd {sl.std():.3f}"
          f"  range [{sl.min():+.3f}, {sl.max():+.3f}]")
    print(f"  proposal predicts -3 ; s*-transform of sqrt(2Dt) predicts about -1")
    print(f"  measured / vHW-form prefactor : median {np.median(rv):.3f}"
          f"  range [{rv.min():.3f}, {rv.max():.3f}]")

    print("\nC.2  species-dependent blur on M2-10")
    c2_species_dependent_blur()
