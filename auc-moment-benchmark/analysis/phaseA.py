"""
Phase A -- simulator trust.

A.1  Validate the reference Lamm solver against the Faxen/Fujita approximate
     analytical solution in the regime where the latter holds.
A.2  Grid convergence measured ON THE MOMENTS, not on the profile.
B.1 / Gate G0  Weak-form vs naive (differentiate-then-integrate) extraction.
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "forward"))

import numpy as np
import lamm, models, moments as M


def a1_faxen(rpm=45000, s_sv=4.0, verbose=True):
    """Max |deviation| between the FV solver and Faxen, boundary clear of ends."""
    s = s_sv * 1e-13
    D, _ = models.d_from_s(s)
    rm, rb = 5.90, 7.20
    grid = lamm.LammGrid(rm, rb, 8000)
    times = np.array([2000.0, 4000.0, 6000.0, 8000.0])
    c = lamm.solve_species(grid, s, D, rpm, times)
    rows = []
    for k, t in enumerate(times):
        fx = lamm.faxen(grid.r, s, D, rpm, t, c0=1.0, rm=rm)
        # compare only where Faxen is valid: boundary well clear of both ends
        w2 = lamm.omega(rpm) ** 2
        rbnd = rm * np.exp(s * w2 * t)
        halfwidth = 8 * np.sqrt(4 * D * t)
        sel = (grid.r > rm + 0.05) & (grid.r < rb - 0.15) & \
              (np.abs(grid.r - rbnd) < halfwidth)
        dev = np.abs(c[k] - fx)[sel].max()
        rows.append((t, rbnd, dev))
        if verbose:
            print(f"  t={t:7.0f}s  r_bnd={rbnd:.4f}  max|FV - Faxen| = {dev:.3e}")
    return rows


# Window placement and the observable time window live in forward/windowing.py
# so that the handoff package can reproduce them without the analysis scripts.
from windowing import place_window, observable_window


def usable_scans(geom, model, **kw):
    """Scans for which an admissible window exists (spec B.3)."""
    return observable_window(geom, model, **kw)


def a2_grid_convergence(m_max=10, levels=(1500, 3000, 6000, 12000)):
    """Moment error vs refinement, on noise-free S1 output."""
    ts = models.test_set()
    model = ts["S1"]
    geom = models.RunGeometry()
    usable = usable_scans(geom, model)
    idx = [u[0] for u in usable]
    wins = {u[0]: u[2] for u in usable}
    ref = None
    results = {}
    for n in levels:
        scans = models.simulate(model, geom, n_cells=n)
        mus = []
        for i in idx:
            mu = M.extract_moments(scans[i:i + 1], geom.r, [geom.times[i]],
                                   geom.rpm, geom.meniscus, wins[i], m_max)
            mus.append(mu[0])
        results[n] = np.array(mus)
    finest = results[levels[-1]]
    table = {}
    for n in levels[:-1]:
        rel = np.abs(results[n] - finest) / np.maximum(np.abs(finest), 1e-30)
        table[n] = rel.max(axis=0)          # worst over scans, per moment order
    return table, results, usable


def g0_weak_vs_naive(m_max=8, n_cells=12000):
    """Gate G0: by-parts and naive extraction must agree on noise-free data."""
    ts = models.test_set()
    model = ts["S1"]
    geom = models.RunGeometry()
    scans = models.simulate(model, geom, n_cells=n_cells)
    usable = usable_scans(geom, model)
    rels = []
    for i, t, w in usable:
        a = M.extract_moments(scans[i:i + 1], geom.r, [t], geom.rpm,
                              geom.meniscus, w, m_max)[0]
        b = M.extract_moments_naive(scans[i:i + 1], geom.r, [t], geom.rpm,
                                    geom.meniscus, w, m_max)[0]
        rels.append(np.abs(a - b) / np.maximum(np.abs(b), 1e-30))
    return np.array(rels), usable


if __name__ == "__main__":
    print("A.1  FV solver vs Faxen analytical")
    a1_faxen()

    print("\nB.3  usable scans (window plateau must cover the s* support)")
    geom = models.RunGeometry()
    for mid, model in models.test_set().items():
        u = usable_scans(geom, model)
        if u:
            print(f"  {mid:10s} {len(u):3d}/{geom.n_scans} scans, "
                  f"t = {u[0][1]:.0f}..{u[-1][1]:.0f} s")
        else:
            print(f"  {mid:10s}   0/{geom.n_scans} scans  <-- NO USABLE WINDOW")

    print("\nG0  weak form vs naive differentiation (noise-free, S1)")
    rels, usable = g0_weak_vs_naive()
    for m in range(rels.shape[1]):
        print(f"   m={m:2d}  max rel diff = {rels[:, m].max():.3e}")
