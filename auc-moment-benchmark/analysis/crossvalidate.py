"""
Cross-validate UltraScan's ASTFEM solver against forward/lamm.py.

This is spec Phase A's real intent: the Python reference solver and the
production solver must agree well below the noise floor, or every downstream
number is a solver artefact.

Prerequisite -- build and run the Layer-1 generator first:

    cd generate && cmake -S . -B build && cmake --build build -j
    ./build/generate /tmp/us3out

then:  python3 analysis/crossvalidate.py /tmp/us3out
"""
import os, sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "forward"))

import numpy as np

import lamm, models, moments as M
from forward_map import estimate_moments
from windowing import observable_window
from phaseC import fit_loglog, law_vhw

MODELS = ("F-S1", "F-M2-10", "F-M3")


def load_scans(outdir, mid):
    """Read the generator's ASCII dump: header of radii, then time + readings."""
    rows = open(os.path.join(outdir, f"{mid}.scans.csv")).read().strip().split("\n")
    r = np.array([float(x) for x in rows[0].split(",")[1:]])
    d = np.array([[float(x) for x in ln.split(",")] for ln in rows[1:]])
    return r, d[:, 0], d[:, 1:]


def reference(model, r, times, n_cells=6000):
    """forward/lamm.py on the same radial grid and the same scan times."""
    grid = lamm.LammGrid(r[0], 7.20, n_cells)
    tot = np.zeros((times.size, grid.n))
    for comp in model["components"]:
        tot += lamm.solve_species(grid, comp["s"], comp["D"], 40000, times,
                                  c0=comp["c"])
    return np.array([np.interp(r, grid.r, tot[i]) for i in range(times.size)])


def geometry_for(r, times, rpm=40000):
    g = models.RunGeometry(rpm=rpm)
    g.times, g.r = times, r
    return g


def main(outdir):
    ts = models.feasible_test_set()

    print("A.1/A.2  concentration profiles, inside the observable window")
    print(f"  {'model':10s} {'scans':>6s} {'max|ASTFEM-lamm|':>18s} {'rel. to plateau':>16s}")
    for mid in MODELS:
        model = ts[mid]
        r, t, c_ast = load_scans(outdir, mid)
        c_lam = reference(model, r, t)
        g = geometry_for(r, t)
        wins = observable_window(g, model)
        idx = [i for i, _, _ in wins]
        lo = min(w.r_lo for _, _, w in wins)
        hi = max(w.r_hi for _, _, w in wins)
        sel = (r >= lo) & (r <= hi)
        dev = np.abs(c_ast[idx][:, sel] - c_lam[idx][:, sel]).max()
        plateau = sum(c["c"] for c in model["components"])
        print(f"  {mid:10s} {len(idx):6d} {dev:18.3e} {dev / plateau:16.2%}")

    print("\nA.2  the quantity that actually matters: the moment vectors")
    print("     (spec asks solver error to sit >=10x below noise error)")
    hdr = " ".join(f"m={m:<2d}  " for m in range(0, 13, 2))
    print(f"  {'model':10s} {hdr}")
    for mid in MODELS:
        model = ts[mid]
        r, t, c_ast = load_scans(outdir, mid)
        c_lam = reference(model, r, t)
        g = geometry_for(r, t)
        w = observable_window(g, model)
        y_a = estimate_moments(c_ast, model, g, 12, windows=w)
        y_l = estimate_moments(c_lam, model, g, 12, windows=w)
        rel = np.abs(y_a - y_l) / np.abs(y_l)
        print(f"  {mid:10s} " + " ".join(f"{rel[m]:.1e}" for m in range(0, 13, 2)))

    print("\nG1  sigma^2(t) scaling re-measured on ASTFEM output (F-S1)")
    model = ts["F-S1"]
    r, t, c_ast = load_scans(outdir, "F-S1")
    g = geometry_for(r, t)
    tt, s2 = [], []
    for i, time, win in observable_window(g, model):
        mu = M.extract_moments(c_ast[i:i + 1], r, [time], 40000, r[0], win, 2)[0]
        _, var = M.central_moments(mu)
        if var > 0:
            tt.append(time); s2.append(var)
    tt, s2 = np.array(tt), np.array(s2)
    slope, se, _ = fit_loglog(tt, s2)
    comp = model["components"][0]
    ratio = np.median(s2 / law_vhw(tt, comp["s"], comp["D"], 40000, r[0]))
    print(f"  exponent               = {slope:+.3f} +- {se:.3f}"
          f"   (lamm.py: -1.073 +- 0.004; proposal: -3)")
    print(f"  measured/vHW prefactor = {ratio:.3f}"
          f"                 (lamm.py: 1.090)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/us3out")
