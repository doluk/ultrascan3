"""
The forward map, and the estimation pipeline.

Two separate things live here, and the distinction matters to the
mathematician reading the handoff package:

1. `moments_of_measure` -- the *algebraic* forward map

       y_k = sum_i c_i s_i^k ,   k = 0..N-1

   This is the map whose inverse is the actual research question: given a
   noisy y with covariance Sigma, recover the atoms (s_i, c_i) and certify
   their number.  It has no AUC content whatsoever.

2. `estimate_moments` -- the *physical* pipeline that produces y-hat from
   simulated scans: weak-form extraction over the observable window, Hermite
   deconvolution of the diffusion blur, averaging over usable scans.

The gap between the two is what Phase D measures: bias = E[y-hat] - y_true,
and the covariance Sigma of y-hat.

Nothing here imports UltraScan or Qt.
"""

import numpy as np

import moments as M
from windowing import observable_window, blur_sigma

SV = 1e-13


# --------------------------------------------------------------------------
# 1. Algebraic forward map
# --------------------------------------------------------------------------

def moments_of_measure(s, c, n_moments):
    """
    y_k = sum_i c_i s_i^k for k = 0 .. n_moments-1.

    s : atom locations [Svedberg]
    c : atom weights (non-negative)
    """
    s = np.atleast_1d(np.asarray(s, float))
    c = np.atleast_1d(np.asarray(c, float))
    k = np.arange(n_moments)
    return (c[:, None] * s[:, None] ** k[None, :]).sum(axis=0)


def truth_moments(model, n_moments, geom=None):
    """
    Ground-truth moment vector for a test-set model.

    In band mode the weight carried by a species is not its signal
    concentration (which is the PEAK of the lamella) but the mass the
    estimator actually integrates, c_i * Phi' with
    Phi' = \int (r0/rm) phi(r0) dr0.  Phi' is a geometric constant shared by
    every species, so it rescales y_0 and leaves the atom locations alone.
    """
    s = [comp["s"] / SV for comp in model["components"]]
    c = [comp["c"] for comp in model["components"]]
    if geom is not None and getattr(geom, "band", False):
        from band import lamella_mass
        phi = lamella_mass(geom.r, geom.meniscus, band_volume=geom.band_volume)
        c = [ci * phi for ci in c]
    return moments_of_measure(s, c, n_moments)


def hankel(y, R=None):
    """
    Hankel matrix H_{ij} = y_{i+j}.  Its rank is the number of atoms, which
    is what a rank certificate would have to establish.
    """
    y = np.asarray(y, float)
    N = y.size
    if R is None:
        R = (N + 1) // 2
    cols = N - R + 1
    return np.array([[y[i + j] for j in range(cols)] for i in range(R)])


# --------------------------------------------------------------------------
# 2. Physical pipeline
# --------------------------------------------------------------------------

def _representative(model):
    comps = model["components"]
    wsum = sum(c["c"] for c in comps) or 1.0
    s_ref = sum(c["c"] * c["s"] for c in comps) / wsum
    D_ref = sum(c["c"] * c["D"] for c in comps) / wsum
    if s_ref <= 0:
        s_ref = float(np.mean([c["s"] for c in comps]))
        D_ref = float(np.mean([c["D"] for c in comps]))
    return s_ref, D_ref


def estimate_moments(scans, model, geom, m_max, k_sigma=4.0,
                     ti_project=False, per_scan=False, windows=None):
    """
    Full pipeline: scans -> windowed weak-form moments -> Hermite
    deconvolution -> averaged moment estimate y-hat (length m_max+1).

    The deconvolution width sigma(t) is the Phase C.1 law evaluated at a
    concentration-weighted mean D.  For a mixture this is a genuine model
    error (each species has its own D); Phase C.2 quantifies it.
    """
    if windows is None:
        windows = observable_window(geom, model, k_sigma=k_sigma)
    if not windows:
        raise ValueError("no usable scans: the observable window is empty")

    s_ref, D_ref = _representative(model)
    idx = [i for i, _, _ in windows]

    if getattr(geom, "band", False):
        return _estimate_band(scans, model, geom, m_max, windows, s_ref, D_ref,
                              per_scan)

    ys = []
    for i, t, w in windows:
        if ti_project:
            a = scans[idx] - scans[idx].mean(axis=0, keepdims=True)
            row = a[idx.index(i)][None, :]
        else:
            row = scans[i:i + 1]
        mu = M.extract_moments(row, geom.r, [t], geom.rpm, geom.meniscus,
                               w, m_max)[0]
        sig = blur_sigma(t, s_ref, D_ref, geom.rpm, geom.meniscus)
        ys.append(M.hermite_deconvolve(mu, sig))
    ys = np.array(ys)
    return ys if per_scan else ys.mean(axis=0)


def _estimate_band(scans, model, geom, m_max, windows, s_ref, D_ref, per_scan):
    """
    Band-mode pipeline.

    Two differences from SV, both of them simplifications:

      * moments come straight from the scan (band.band_moment_kernel), with
        no integration by parts and no derivative;
      * the lamella's footprint is removed EXACTLY by its own known moments
        before the Gaussian diffusion blur is undone by the Hermite step.
    """
    import band as B

    ys = []
    for i, t, w in windows:
        mu = B.extract_moments_band(scans[i:i + 1], geom.r, [t], geom.rpm,
                                    geom.meniscus, w, m_max)[0]
        # exact: the lamella shape and width are known before the experiment
        e = B.lamella_eps_moments(geom.r, geom.meniscus, t, geom.rpm,
                                  m_max + 1, band_volume=geom.band_volume)
        mu = B.deconvolve_known(mu, e)
        # approximate: diffusion, assumed Gaussian with the measured law
        sig = B.blur_sigma_band(t, s_ref, D_ref, geom.rpm, geom.meniscus, 0.0)
        ys.append(M.hermite_deconvolve(mu, sig))
    ys = np.array(ys)
    return ys if per_scan else ys.mean(axis=0)
