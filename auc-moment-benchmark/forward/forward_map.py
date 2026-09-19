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


def truth_moments(model, n_moments):
    """Ground-truth moment vector for a test-set model."""
    s = [comp["s"] / SV for comp in model["components"]]
    c = [comp["c"] for comp in model["components"]]
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
