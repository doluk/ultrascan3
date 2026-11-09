#!/usr/bin/env python3
"""
c(s) analysis (Schuck 2000 type) of 2D runs, for comparison with published
misalignment data that were analysed with SEDFIT c(s).

  * Data: iso-radius lateral mean of the total signal (analysis2d.py), with
    Gaussian noise sigma = --noise * c_total (fixed seed, identical for all
    runs).  The first scan is skipped.
  * Kernels: ideal planar 1D Lamm solutions in the geometry of the 2D model
    (Scharfetter-Gummel FV, BDF2), D(s) from a common f/f0 (Svedberg).
  * Inversion: NNLS with second-derivative Tikhonov regularisation; lambda is
    set so that chi^2 / chi^2_min = 1 + p/(n-p) F_{p,n-p}(P), P = 0.68.
  * f/f0: fitted (bounded scalar minimisation of the unregularised rmsd) or
    fixed (--ff0, Channell et al. 2018 used 1.25).

Usage: cs_analysis.py [--ff0 1.25] [--noise 0.003] run1.npz [run2.npz ...]
Writes <run>.cs.json (distribution and peak statistics).
"""
import sys, json, argparse
import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
from scipy.optimize import nnls, minimize_scalar
from scipy.stats import f as fdist
from misalign1d import bernoulli
from analysis2d import iso_radius_mean

KT = 1.380649e-16 * 293.15
ETA, VBAR, RHO = 0.01002, 0.73, 1.0


def D_of_s(s, ff0):
    """Svedberg + Stokes: D for sedimentation coefficient s (sec) and f/f0."""
    m23 = s * 6 * np.pi * ETA * ff0 * (3 * VBAR / (4 * np.pi)) ** (1 / 3) / (1 - VBAR * RHO)
    m = m23 ** 1.5
    return KT * s / (m * (1 - VBAR * RHO))


def kernel(s, D, times, psi_deg, r_m, L, rpm, N=360, dt=5.0):
    """Planar Lamm solution (c0 = 1) at the given times, cell centres."""
    w2 = (rpm * np.pi / 30.) ** 2
    cp = np.cos(np.deg2rad(psi_deg))
    xf = np.linspace(0, L, N + 1); xc = 0.5 * (xf[1:] + xf[:-1]); h = L / N
    v = s * w2 * (r_m + xf[1:-1] * cp) * cp
    Pe = v * h / D
    a, b = D / h * bernoulli(-Pe), D / h * bernoulli(Pe)      # F = a c_l - b c_r
    main = np.zeros(N); main[:-1] -= a; main[1:] -= b
    A = sp.diags([main, b, a], [0, 1, -1]) / h               # dc/dt = A c
    I = sp.identity(N)
    lu1 = spla.splu((I - dt * A).tocsc())
    lu2 = spla.splu((1.5 * I - dt * A).tocsc())
    c_old, c = None, np.ones(N)
    out, t, k = [], 0.0, 0
    nsteps = int(round(times[-1] / dt))
    tidx = {int(round(tt / dt)): j for j, tt in enumerate(times)}
    res = np.empty((len(times), N))
    for n in range(1, nsteps + 1):
        if c_old is None:
            c_new = lu1.solve(c)
        else:
            c_new = lu2.solve(2.0 * c - 0.5 * c_old)
        c_old, c = c, c_new
        if n in tidx:
            res[tidx[n]] = c
    return xc, res


class CS:
    def __init__(self, x, data, times, psi_deg, r_m, L, rpm, sgrid, mask):
        self.x, self.data, self.t = x, data, times
        self.psi, self.r_m, self.L, self.rpm = psi_deg, r_m, L, rpm
        self.sg, self.mask = sgrid, mask
        self.y = data[:, mask].ravel()

    def design(self, ff0):
        cols = []
        for s in self.sg:
            xk, K = kernel(s * 1e-13, D_of_s(s * 1e-13, ff0), self.t, self.psi,
                           self.r_m, self.L, self.rpm)
            cols.append(np.array([np.interp(self.x[self.mask], xk, Kj) for Kj in K]).ravel())
        return np.array(cols).T

    def solve(self, ff0, regularise=True, P=0.68):
        A = self.design(ff0)
        w0, r0 = nnls(A, self.y, maxiter=20 * A.shape[1])
        chi0 = r0 ** 2
        if not regularise:
            return w0, np.sqrt(chi0 / len(self.y)), A
        n, p = A.shape
        target = chi0 * (1 + p / (n - p) * fdist.ppf(P, p, n - p))
        Dm = np.diff(np.eye(p), 2, axis=0)
        sc = np.linalg.norm(A, 2)
        lo, hi = -8.0, 2.0
        for _ in range(40):                       # bisection on log10(lambda)
            mid = 0.5 * (lo + hi)
            lam = 10 ** mid * sc
            w, _ = nnls(np.vstack([A, lam * Dm]), np.r_[self.y, np.zeros(p - 2)], maxiter=20 * p)
            chi = ((A @ w - self.y) ** 2).sum()
            if chi > target:
                hi = mid
            else:
                lo = mid
        lam = 10 ** lo * sc
        w, _ = nnls(np.vstack([A, lam * Dm]), np.r_[self.y, np.zeros(p - 2)], maxiter=20 * p)
        return w, np.sqrt(((A @ w - self.y) ** 2).mean()), A


def peaks(sg, w, split=5.4, hi=8.8):
    ds = sg[1] - sg[0]
    tot = w.sum()
    m1 = (sg < split); m2 = (sg >= split) & (sg < hi); m3 = sg >= hi
    def mom(m):
        if w[m].sum() <= 0:
            return np.nan, np.nan
        mu = (sg[m] * w[m]).sum() / w[m].sum()
        return mu, np.sqrt((((sg[m] - mu) ** 2) * w[m]).sum() / w[m].sum())
    s1, sd1 = mom(m1); s2, sd2 = mom(m2)
    return dict(s_mono=s1, sd_mono=sd1, s_dimer=s2, sd_dimer=sd2,
                f_mono=w[m1].sum() / tot, f_dimer=w[m2].sum() / tot, f_higher=w[m3].sum() / tot,
                h_mono=w[m1].max() / (tot * ds), h_dimer=(w[m2].max() / (tot * ds)) if m2.any() else np.nan,
                total=tot)


def analyse(fn, ff0_fixed=None, noise=0.003, seed=1):
    d = np.load(fn)
    a = json.loads(str(d["args"]))
    C = d["C"]
    if C.ndim == 4:
        C = C.sum(axis=1)                         # total signal (equal extinction per mass)
    ctot = float(np.array(d["species"])[:, 2].sum()) if "species" in d.files else a["c0"]
    t, x = d["t"], d["x"]
    cbar = iso_radius_mean(C / ctot, x, d["y"], a["psi"], W=0.28, wts=np.diff(d["yn"]))
    rng = np.random.default_rng(seed)
    cbar = cbar + noise * rng.standard_normal(cbar.shape)
    sel = slice(1, None)
    L, r_m, rpm = a.get("L", 0.9), a.get("r_m", 6.2), a.get("rpm", 50000.)
    mask = (x > 0.02) & (x < L - 0.06)
    sg = np.round(np.arange(1.0, 12.01, 0.1), 2)
    cs = CS(x, cbar[sel], t[sel], a["psi"], r_m, L, rpm, sg, mask)
    if ff0_fixed is None:
        opt = minimize_scalar(lambda f: cs.solve(f, regularise=False)[1], bounds=(1.0, 2.2),
                              method="bounded", options=dict(xatol=2e-3))
        ff0 = opt.x
    else:
        ff0 = ff0_fixed
    w, rmsd, _ = cs.solve(ff0)
    out = dict(file=fn, psi=a["psi"], ff0=ff0, ff0_fixed=ff0_fixed is not None, noise=noise,
               rmsd=rmsd, s=sg.tolist(), c=(w / (sg[1] - sg[0])).tolist())
    out.update(peaks(sg, w))
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--ff0", type=float, default=None)
    ap.add_argument("--noise", type=float, default=0.003)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    for fn in a.files:
        r = analyse(fn, a.ff0, a.noise)
        tag = f"ff{a.ff0:g}" if a.ff0 else "fit"
        json.dump(r, open(fn.replace(".npz", f".cs_{tag}.json"), "w"), indent=1)
        print(f"{fn:22s} psi={r['psi']:.0f} f/f0={r['ff0']:.3f} rmsd={r['rmsd']:.2e} | "
              f"mono s={r['s_mono']:.3f} sd={r['sd_mono']:.3f} h={r['h_mono']:.2f} f={100*r['f_mono']:.1f}% | "
              f"dimer s={r['s_dimer']:.3f} sd={r['sd_dimer']:.3f} f={100*r['f_dimer']:.1f}% | "
              f">8.8S {100*r['f_higher']:.1f}%", flush=True)
