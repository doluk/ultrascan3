"""
Phase E -- comparators and stress tests.

E.1 is the honest comparison.  The proposal's Phase-1 milestone
(delta-s/s = 15%, <1% error) is something c(s)/2DSA already do routinely, so
hitting it proves nothing.  What matters is whether there is a separation at
which the moment route WINS.

The 2DSA comparator here is a stand-in, not UltraScan's us_2dsa: a
non-negative least squares fit over an s-grid of Lamm solutions at fixed
f/f0, with time-invariant and radially-invariant noise eliminated
analytically.  That is the c(s) analogue (spec E.1 item 2) and is the right
baseline to beat.  Running the real us_2dsa on the same .auc files is Layer-1
work and is listed as an open item in FINDINGS.md.
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "forward"))

import numpy as np
from scipy.optimize import nnls

import models, noise as noisemod, lamm
from forward_map import estimate_moments, truth_moments, hankel
from windowing import observable_window

SV = 1e-13


# --------------------------------------------------------------------------
# TI / RI elimination
# --------------------------------------------------------------------------

def double_center(X):
    """
    Remove the time-invariant and radially-invariant subspaces from X.

    A model  a(r,t) = A(r,t) + psi(r) + beta(t)  with psi, beta entirely free
    is a two-way additive layout, so the least-squares elimination of psi and
    beta is exactly two-way centering: subtract row and column means and add
    back the grand mean.  Applying the same projector to the data and to every
    basis function gives the profiled least-squares problem in A alone.
    """
    X = np.asarray(X, float)
    return X - X.mean(0, keepdims=True) - X.mean(1, keepdims=True) + X.mean()


# --------------------------------------------------------------------------
# c(s)-style NNLS comparator
# --------------------------------------------------------------------------

def fit_mask(geom, lo_pad=0.02, hi_pad=0.10):
    """
    Radial range used by the comparator.

    The bottom pileup is a thin, very steep layer that neither a real
    instrument nor a fitting basis represents well, and standard practice is
    to exclude it; leaving it in makes NNLS dump compensating mass at the
    edge of the s-grid and mis-resolve peaks even on noise-free data.  The
    moment pipeline is already restricted to a window strictly inside the
    cell, so excluding it here keeps the comparison fair rather than
    favouring either side.
    """
    return (geom.r > geom.meniscus + lo_pad) & (geom.r < geom.bottom - hi_pad)


def cs_basis(geom, s_grid_sv, f_f0=1.25, n_cells=6000, eliminate=True,
             mask=None):
    """
    Design matrix of Lamm solutions over `s_grid_sv` (Svedbergs) at fixed
    f/f0, with the TI/RI subspaces projected out.

    The basis depends only on the experiment, not on the noise, so it is
    built once and reused across every Monte Carlo draw.
    """
    if mask is None:
        mask = fit_mask(geom)
    grid = lamm.LammGrid(geom.meniscus, geom.bottom, n_cells)
    cols = []
    for s_sv in s_grid_sv:
        comp = models.species(s_sv, 1.0, f_f0=f_f0)
        c = lamm.solve_species(grid, comp["s"], comp["D"], geom.rpm, geom.times)
        prof = np.empty((len(geom.times), geom.r.size))
        for i in range(c.shape[0]):
            prof[i] = np.interp(geom.r, grid.r, c[i])
        prof = prof[:, mask]
        cols.append(double_center(prof).ravel() if eliminate else prof.ravel())
    return np.array(cols).T


def cs_nnls(scans, A, mask, eliminate=True):
    """Non-negative fit of `scans` onto a prebuilt design matrix `A`."""
    X = np.asarray(scans)[:, mask]
    b = (double_center(X) if eliminate else X).ravel()
    x, _ = nnls(A, b)
    return x


def peaks_from_distribution(s_grid, amp, n_peaks):
    """Centre of mass of the `n_peaks` largest contiguous groups of amplitude."""
    amp = np.asarray(amp, float)
    if amp.sum() <= 0:
        return np.full(n_peaks, np.nan)
    # split into contiguous non-zero runs
    nz = amp > amp.max() * 1e-3
    runs, start = [], None
    for i, v in enumerate(nz):
        if v and start is None:
            start = i
        elif not v and start is not None:
            runs.append((start, i)); start = None
    if start is not None:
        runs.append((start, len(nz)))
    runs.sort(key=lambda ab: -amp[ab[0]:ab[1]].sum())
    out = []
    for a, b in runs[:n_peaks]:
        w = amp[a:b]
        out.append(float((s_grid[a:b] * w).sum() / w.sum()))
    while len(out) < n_peaks:
        out.append(np.nan)
    return np.array(sorted(out))


# --------------------------------------------------------------------------
# Prony / ESPRIT from the moment vector
# --------------------------------------------------------------------------

def prony(y, R):
    """
    Recover R atoms (s_i, c_i) from moments y_0..y_{>=2R-1}.

    Classical Prony: the atoms are the generalised eigenvalues of the pencil
    (H1, H0) built from shifted Hankel matrices.  Weights then follow from a
    Vandermonde solve.  No regularisation, no covariance weighting -- just
    enough to see whether the information is present.
    """
    y = np.asarray(y, float)
    if y.size < 2 * R:
        raise ValueError("need at least 2R moments")
    H0 = np.array([[y[i + j] for j in range(R)] for i in range(R)])
    H1 = np.array([[y[i + j + 1] for j in range(R)] for i in range(R)])
    try:
        from scipy.linalg import eig
        vals = eig(H1, H0, right=False)
    except Exception:
        return np.full(R, np.nan), np.full(R, np.nan)
    s = np.real(vals[np.isfinite(vals)])
    s = np.sort(s)
    if s.size < R:
        return np.full(R, np.nan), np.full(R, np.nan)
    V = np.vander(s, N=len(y), increasing=True).T
    c, *_ = np.linalg.lstsq(V, y, rcond=None)
    return s, c


def hankel_spectrum(y, R=None):
    """Singular values of the Hankel matrix, normalised by the largest."""
    H = hankel(y, R)
    sv = np.linalg.svd(H, compute_uv=False)
    return sv / sv[0]


# --------------------------------------------------------------------------
# Drivers
# --------------------------------------------------------------------------

def e1_crossover(n_mc=12, preset="absorbance", ti_mode="oracle", m_max=8,
                 rpm=40000, seed=1):
    """
    Recovered (s1, s2) error vs delta-s/s, for the c(s)-style NNLS comparator
    and for Prony on the moment vector.

    The moment route is given every advantage here: absorbance optics and
    ORACLE removal of the time-invariant noise.  If it cannot win under those
    conditions it will not win under real ones.
    """
    from forward_map import estimate_moments

    geom = models.RunGeometry(rpm=rpm, n_scans=40)
    ts = models.feasible_test_set()
    s0 = models.S0_FEASIBLE
    rows = []
    for tag, ds in (("15", 0.15), ("10", 0.10), ("05", 0.05), ("03", 0.03)):
        mid = f"F-M2-{tag}"
        model = ts[mid]
        truth = np.array(sorted(c["s"] / SV for c in model["components"]))
        clean = models.simulate(model, geom)
        windows = observable_window(geom, model)
        gen = noisemod.NoiseGenerator(geom.r, len(geom.times), preset=preset,
                                      seed=seed)
        s_grid = np.linspace(s0 * 0.85, s0 * 1.35, 60)
        mask = fit_mask(geom)
        A = cs_basis(geom, s_grid, mask=mask)

        err_cs, err_pr = [], []
        for k in range(n_mc):
            noisy, ti = gen.draw(clean, return_parts=True)
            if ti_mode == "oracle":
                noisy = noisy - ti[None, :]

            amp = cs_nnls(noisy, A, mask)
            pk = peaks_from_distribution(s_grid, amp, 2)
            err_cs.append(np.abs(pk - truth) / truth)

            y = estimate_moments(noisy, model, geom, m_max, windows=windows)
            s_pr, c_pr = prony(y, 2)
            s_pr = np.sort(s_pr)
            err_pr.append(np.abs(s_pr - truth) / truth)

        rows.append(dict(tag=tag, ds=ds, truth=truth,
                         cs=np.nanmedian(np.array(err_cs), axis=0),
                         prony=np.nanmedian(np.array(err_pr), axis=0)))
    return rows


def e2_rank_gap(m_max=12, preset="absorbance", ti_mode="oracle", seed=3,
                rpm=40000):
    """
    Hankel singular-value spectrum for atomic and non-atomic truths.

    The failure mode being probed: does a distribution with NO rank produce a
    confident-looking rank gap anyway?
    """
    from forward_map import estimate_moments

    geom = models.RunGeometry(rpm=rpm, n_scans=40)
    ts = models.feasible_test_set()
    out = {}
    for mid in ("F-S1", "F-M2-10", "F-M3", "F-BROAD", "F-RA-fast"):
        model = ts[mid]
        windows = observable_window(geom, model)
        if not windows:
            out[mid] = None
            continue
        clean = models.simulate(model, geom)
        gen = noisemod.NoiseGenerator(geom.r, len(geom.times), preset=preset,
                                      seed=seed)
        noisy, ti = gen.draw(clean, return_parts=True)
        if ti_mode == "oracle":
            noisy = noisy - ti[None, :]
        y = estimate_moments(noisy, model, geom, m_max, windows=windows)
        y_true = truth_moments(model, m_max + 1)
        # normalise the support to [-1,1] before forming the Hankel matrix,
        # otherwise the s^m dynamic range alone dictates the spectrum
        out[mid] = dict(noisy=hankel_spectrum(_rescale(y, model)),
                        clean=hankel_spectrum(_rescale(y_true, model)),
                        atomic=model["atomic"],
                        n_atoms=sum(1 for c in model["components"] if c["c"] > 0))
    return out


def _rescale(y, model, n=None):
    """
    Re-express moments on a support rescaled to [-1, 1].

    y_k = sum c_i s_i^k on [a,b] -> moments of the pushforward under
    x = (2s - (a+b))/(b-a), computed by the binomial transform.
    """
    from math import comb
    a, b = models.support_bounds(model)
    alpha, beta = 2.0 / (b - a), -(a + b) / (b - a)
    N = len(y) if n is None else n
    out = np.zeros(N)
    for k in range(N):
        out[k] = sum(comb(k, j) * (alpha ** j) * (beta ** (k - j)) * y[j]
                     for j in range(k + 1))
    return out


if __name__ == "__main__":
    print("E.1  recovered (s1,s2) median relative error vs delta-s/s")
    print("     (absorbance optics, ORACLE TI removal -- best case for moments)")
    print(f"  {'ds/s':>6}  {'c(s) NNLS s1':>13} {'s2':>10}   {'Prony s1':>10} {'s2':>10}")
    for r in e1_crossover():
        print(f"  {r['ds']:6.0%}  {r['cs'][0]:13.2%} {r['cs'][1]:10.2%}   "
              f"{r['prony'][0]:10.2%} {r['prony'][1]:10.2%}")

    print("\nE.2  Hankel singular-value spectrum (normalised support)")
    for mid, d in e2_rank_gap().items():
        if d is None:
            print(f"  {mid:12s} no usable window")
            continue
        kind = f"atomic R={d['n_atoms']}" if d["atomic"] else "NON-ATOMIC"
        sv = " ".join(f"{v:.1e}" for v in d["noisy"][:7])
        svc = " ".join(f"{v:.1e}" for v in d["clean"][:7])
        print(f"  {mid:12s} {kind:14s}")
        print(f"       truth {svc}")
        print(f"       noisy {sv}")
