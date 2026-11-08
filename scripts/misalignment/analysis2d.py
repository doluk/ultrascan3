#!/usr/bin/env python3
"""
Analysis of 2D runs (misalign2d_implicit.py): the lateral-average profiles
are fitted with a planar 1D model containing the misalignment closure,

  dc/dt = -d/dx[ (v + u_d) c - D dc/dx ] - k_a c      (+ deposit at bottom)
  v(x)   = s w^2 (r_m + x cos psi) cos psi
  u_d(x) = kappa_d Int_x^L s w^2 (r_m + x' cos psi) sin psi dx' / W
  k_a(x) = kappa_a s w^2 (r_m + x cos psi) sin psi / W

(the planar analogue of misalign1d.py with the two wall-layer branches
separated: kappa_a = enriched layer carried to the bottom, kappa_d =
depleted layer carried to the boundary; the PNK closure is kappa_a =
kappa_d).  Fits: (s, D) with no closure; (s, D, kappa_a, kappa_d).  Usage:  analysis2d.py run1.npz [run2.npz ...]
"""
import sys, json
import numpy as np
from scipy.integrate import solve_ivp
from scipy.optimize import least_squares
from misalign1d import bernoulli


def planar1d(s, D, kappa_a, kappa_d, psi_deg, times, r_m=6.2, L=0.9, W=0.28,
             rpm=50000., N=450, c0=1.0):
    w2 = (rpm * np.pi / 30.) ** 2
    ps = np.deg2rad(psi_deg)
    xf = np.linspace(0, L, N + 1); xc = 0.5 * (xf[1:] + xf[:-1]); h = xf[1] - xf[0]
    Gf = w2 * (r_m + xf * np.cos(ps))
    Gc = w2 * (r_m + xc * np.cos(ps))
    # u at internal faces: kappa * s sin(psi) * Int_x^L G dx' / W
    IG = w2 * (r_m * (L - xf) + 0.5 * np.cos(ps) * (L**2 - xf**2))
    u = kappa_d * s * np.sin(ps) * IG[1:-1] / W
    k = kappa_a * s * np.sin(ps) * Gc / W
    v = s * Gf[1:-1] * np.cos(ps) + u
    Pe = v * h / D
    Bm, Bp = bernoulli(-Pe), bernoulli(Pe)

    def rhs(t, c):
        F = D / h * (Bm * c[:-1] - Bp * c[1:])
        dc = np.zeros_like(c)
        dc[:-1] -= F; dc[1:] += F
        dc /= h
        loss = k * c
        dc -= loss
        dc[-1] += loss.sum()
        return dc

    sol = solve_ivp(rhs, (0, times[-1]), np.full(N, c0), t_eval=times,
                    method="LSODA", rtol=1e-7, atol=1e-10)
    return xc, sol.y.T


def fit(x, data, times, psi_deg, free_kappa, guess=(4.4, 6.0, 0.0, 0.0), xmax=None,
        L=0.9, W=0.28, r_m=6.2):
    mask = x < (xmax if xmax else L - 0.06)

    def res(p):
        ka, kd = (p[2], p[3]) if free_kappa else (0.0, 0.0)
        xx, C = planar1d(p[0] * 1e-13, p[1] * 1e-7, ka, kd, psi_deg, times,
                         r_m=r_m, L=L, W=W)
        return np.array([np.interp(x[mask], xx, Ci) for Ci in C]).ravel() - data[:, mask].ravel()

    x0 = list(guess) if free_kappa else list(guess[:2])
    lb = [1, 0.1, -5.0, -5.0][:len(x0)]
    ub = [20, 60, 5.0, 5.0][:len(x0)]
    sol = least_squares(res, x0, bounds=(lb, ub), x_scale="jac", diff_step=1e-4)
    return sol.x, np.sqrt(np.mean(sol.fun ** 2))


def iso_radius_mean(C, x, y, psi_deg, W=0.28):
    """Average c over y along lines of constant radius
    rho = r_m + x cos psi + (y - W/2) sin psi  (what the optics sample),
    returned on the centre-line coordinate x_c = (rho - r_m) / cos psi."""
    ps = np.deg2rad(psi_deg)
    out = np.empty(C.shape[:2])
    for k in range(C.shape[0]):
        cols = []
        for j, yy in enumerate(y):
            # position x at which column j has the radius of centre-line x
            xs = x - (yy - W / 2) * np.tan(ps)
            cols.append(np.interp(xs, x, C[k, :, j]))
        out[k] = np.mean(cols, axis=0)
    return out


def analyse(fn):
    d = np.load(fn)
    a = json.loads(str(d["args"]))
    C = d["C"] / a["c0"]
    t, x = d["t"], d["x"]
    cbar = iso_radius_mean(C, x, d["y"], a["psi"])
    sel = slice(1, None)              # skip the first scan
    p0, r0 = fit(x, cbar[sel], t[sel], a["psi"], False)
    if a["psi"] != 0:
        p1, r1 = fit(x, cbar[sel], t[sel], a["psi"], True, guess=(p0[0], p0[1], 0.3, 0.1))
    else:
        p1, r1 = np.r_[p0, 0.0, 0.0], r0
    # lateral inhomogeneity: rms deviation of c from its lateral mean (plateau rows)
    pl = (x > 0.5) & (x < 0.8)
    lat = np.sqrt(((C[-1][pl] - C[-1][pl].mean(axis=1)[:, None]) ** 2).mean())
    out = dict(file=fn, psi=a["psi"], c0=a["c0"], Nx=a["Nx"], Ny=a["Ny"], dt=a["dt"],
               s_k0=p0[0], D_k0=p0[1], rmsd_k0=r0,
               s_k=p1[0], D_k=p1[1], kappa_a=p1[2], kappa_d=p1[3], rmsd_k=r1,
               lateral_rms=lat)
    return out


if __name__ == "__main__":
    rows = [analyse(f) for f in sys.argv[1:]]
    for r in rows:
        print(f"{r['file']:28s} psi={r['psi']:.2f} c0={r['c0']:.0e} {r['Nx']}x{r['Ny']} dt={r['dt']:g} | "
              f"kappa=0: s={r['s_k0']:.4f} D={r['D_k0']:.3f} rmsd={r['rmsd_k0']:.1e} | "
              f"closure: s={r['s_k']:.4f} D={r['D_k']:.3f} ka={r['kappa_a']:.3f} kd={r['kappa_d']:.3f} rmsd={r['rmsd_k']:.1e} | "
              f"lat.rms={r['lateral_rms']:.1e}")
    out = sys.argv[0].replace("analysis2d.py", "") + "results2d.json"
    try:
        old = {r["file"]: r for r in json.load(open(out))}
    except (OSError, ValueError):
        old = {}
    old.update({r["file"]: r for r in rows})
    json.dump(list(old.values()), open(out, "w"), indent=1)
