"""
Phase D -- noise, the moment covariance, and the gating number N_eff.

The deliverable of the whole prototype is (y, Sigma): the moment vector and
its FULL covariance, not a scalar noise level.  Moments from the same scans
are strongly correlated, so an SDP formulation assuming i.i.d. moment errors
would be solving a different problem.
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "forward"))

import numpy as np
import models, noise as noisemod
from forward_map import estimate_moments, truth_moments
from windowing import observable_window


def monte_carlo(model, geom, m_max=10, n_mc=500, preset="interference",
                seed=0, ti_mode="none", n_cells=6000, clean=None,
                k_sigma=4.0):
    """
    n_mc independent noise realizations through the full pipeline.

    ti_mode selects how time-invariant noise is handled:

      "none"     leave it in -- the honest raw result for interference optics
      "oracle"   subtract the TRUE TI vector.  Not achievable in practice; it
                 is the upper bound on what any TI-removal scheme could give,
                 and is the right thing to quote when asking whether the
                 method is worth pursuing at all
      "timemean" subtract the time-mean of the scans.  This annihilates TI
                 exactly, but it also removes a large part of the signal, so
                 it is biased unless the forward map is projected identically
      "band-gated" estimate TI and RI from the band-free regions (band mode
                 only).  Unlike "oracle" this is ACHIEVABLE: it needs only
                 the a priori support bounds and an upper bound on D

    Returns dict with y_true, realizations, y_hat (mean), bias, Sigma.
    """
    if clean is None:
        clean = models.simulate(model, geom, n_cells=n_cells)
    windows = observable_window(geom, model, k_sigma=k_sigma)
    if not windows:
        return None
    gen = noisemod.NoiseGenerator(geom.r, len(geom.times), preset=preset, seed=seed)

    reals = np.empty((n_mc, m_max + 1))
    for k in range(n_mc):
        noisy, ti_vec = gen.draw(clean, return_parts=True)
        if ti_mode == "oracle":
            noisy = noisy - ti_vec[None, :]
        elif ti_mode == "band-gated":
            import band as _band
            noisy = _band.denoise(noisy, geom, model)[0]
        reals[k] = estimate_moments(noisy, model, geom, m_max,
                                    ti_project=(ti_mode == "timemean"),
                                    windows=windows)
    y_true = truth_moments(model, m_max + 1, geom=geom)
    y_hat = reals.mean(axis=0)
    Sigma = np.cov(reals, rowvar=False)
    return dict(y_true=y_true, realizations=reals, y_hat=y_hat,
                bias=y_hat - y_true, Sigma=Sigma,
                n_scans_used=len(windows), preset=preset,
                ti_mode=ti_mode)


def n_eff(res, tol=0.10):
    """
    Largest m such that the TOTAL relative error stays below `tol` for all
    orders up to m.  R_max = floor((N_eff-1)/2).

    Total error means RMSE = sqrt(Sigma_mm + bias_m^2), not the standard
    deviation alone.  Quoting the standard deviation by itself is how a
    badly biased estimator (time-mean projection, say) can look excellent:
    its scatter is tiny and it is simply estimating the wrong quantity.
    """
    rmse = np.sqrt(np.diag(res["Sigma"]) + res["bias"] ** 2)
    rel = rmse / np.maximum(np.abs(res["y_true"]), 1e-300)
    best = -1
    for m in range(rel.size):
        if rel[m] < tol:
            best = m
        else:
            break
    return best, rel


def correlation(Sigma):
    d = np.sqrt(np.diag(Sigma))
    d = np.maximum(d, 1e-300)
    return Sigma / np.outer(d, d)


if __name__ == "__main__":
    m_max = 14
    n_mc = int(os.environ.get("N_MC", 500))
    geom = models.RunGeometry(rpm=40000)
    ts = models.feasible_test_set()

    print(f"D.2/D.3  Monte Carlo over {n_mc} noise realizations, m_max={m_max}")
    print(f"{'model':12s} {'optics':14s} {'TI':9s} {'scans':6s} "
          f"{'N_eff':6s} {'R_max':6s}   total rel. error by order m=0,2,4,..14")
    for mid in ("F-S1", "F-M2-10", "F-M2-05", "F-M3"):
        model = ts[mid]
        clean = models.simulate(model, geom)
        for preset, tip in (("white-only", "none"), ("absorbance", "none"),
                            ("interference", "none"), ("interference", "oracle"),
                            ("interference", "timemean")):
            if True:
                res = monte_carlo(model, geom, m_max=m_max, n_mc=n_mc,
                                  preset=preset, ti_mode=tip, clean=clean)
                if res is None:
                    print(f"{mid:12s} {preset:14s} -- no usable window --")
                    continue
                N, rel = n_eff(res)
                R = (N - 1) // 2 if N >= 1 else -1
                bars = " ".join(f"{r:.1e}" for r in rel[0:16:2])
                print(f"{mid:12s} {preset:14s} {tip:9s} "
                      f"{res['n_scans_used']:6d} {N:6d} {R:6d}   {bars}")
