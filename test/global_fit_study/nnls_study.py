#!/usr/bin/env python3
"""NNLS-level global-fit study with known truth.

Uses ASTFEM basis matrices exported by gf_basis (real US_SolveSim
simulations of a 2DSA-like s x f/f0 grid that contains the three true
species) and Monte Carlo noise replicates to compare the global-fit variants:

  cur    current us_mpi_analysis:  data divided by the given total
         concentration, OD limit NOT scaled (mask b/c >= ODlimit), unweighted
  F1     OD limit scaled with the data (mask b >= ODlimit)
  W      F1 + rows weighted by 1/sigma_e (oracle sigma) instead of the implicit
         weight 1/c_e of the normalized data
  What   F1 + weights from sigma estimated by a per-dataset fit (two-stage)
  Wa     W + a fitted concentration factor alpha_e per dataset (alpha_0 = 1)

Metrics against the truth:  signal fractions of the three species (s
windows), signal-weighted mean s, and per-dataset total concentration of the
scaled-global (SC) model.

Usage:  nnls_study.py <basis-dir> [--reps N] [--out results.json]
"""

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from scipy.optimize import nnls

TRUE_S = np.array([2.5, 4.5, 7.0])
TRUE_K = np.array([1.25, 1.6, 2.1])
TRUE_F = np.array([0.25, 0.40, 0.30]) / 0.95
WINDOWS = [(0.0, 3.5), (3.5, 5.75), (5.75, 50.0)]


class Basis:
    def __init__(self, bdir, rpm):
        meta = json.loads((Path(bdir) / ("basis_%d.json" % rpm)).read_text())
        self.s = np.array(meta["s"])
        self.k = np.array(meta["ff0"])
        nrows = meta["nscans"] * meta["npoints"]
        raw = np.fromfile(Path(bdir) / ("basis_%d.A.bin" % rpm), dtype="<f8")
        self.A = raw.reshape(len(self.s), nrows).T.copy()
        self.xtrue = np.zeros(len(self.s))
        for s, k, f in zip(TRUE_S, TRUE_K, TRUE_F):
            jj = np.where((np.abs(self.s - s) < 1e-9) & (np.abs(self.k - k) < 1e-9))[0]
            assert len(jj) == 1, (s, k)
            self.xtrue[jj[0]] = f
        self.win = [np.where((self.s >= lo) & (self.s < hi))[0] for lo, hi in WINDOWS]


def stray(od, frac):
    if frac <= 0.0:
        return od
    return -np.log10((10.0 ** (-od) + frac) / (1.0 + frac))


def make_data(basis, c, sigma, frac, rng):
    clean = c * (basis.A @ basis.xtrue)
    return stray(clean, frac) + rng.normal(0.0, sigma, clean.shape)


def qr_block(A, b):
    Q, R = np.linalg.qr(A, mode="reduced")
    return R, Q.T @ b


def solve(blocks):
    """NNLS over stacked blocks given as (w, R_e, Q_e^T b_e, alpha_e):  the
    weighted rows w*A_e, w*b_e/alpha_e of each dataset, reduced by a per-block
    QR (the NNLS solution of the stacked system is unchanged)."""
    Rs = [w * R for (w, R, y, a) in blocks]
    ys = [w * y / a for (w, R, y, a) in blocks]
    x, _ = nnls(np.vstack(Rs), np.concatenate(ys), maxiter=5000)
    return x


def fit_variant(variant, dsets, cgiven, odlim, sig_est=None, n_alpha=15):
    """dsets: list of (basis, b).  Returns x (normalized units), alpha, masks."""
    ne = len(dsets)
    alpha = np.ones(ne)
    masks, scaled, qrs = [], [], []
    for (basis, b), c, ol in zip(dsets, cgiven, odlim):
        bs = b / c
        if variant == "cur":
            m = bs < ol               # unscaled limit against normalized data
        else:
            m = b < ol                # limit in data units (F1)
        masks.append(m)
        scaled.append(bs)
        qrs.append(qr_block(basis.A[m], bs[m]))

    def weights(alpha):
        if variant in ("cur", "F1"):
            return np.ones(ne)        # implicit weight 1/c of normalized data
        # residual of normalized data r/c -> weight c/sigma gives r/sigma
        return np.array([cgiven[e] * alpha[e] / sig_est[e] for e in range(ne)])

    def blocks(alpha):
        w = weights(alpha)
        return [(w[e], qrs[e][0], qrs[e][1], alpha[e]) for e in range(ne)]

    x = solve(blocks(alpha))
    if variant == "Wa":
        for _ in range(n_alpha):
            for e in range(1, ne):    # alpha_0 = 1 fixes the scale
                basis, b = dsets[e]
                m = masks[e]
                p = basis.A[m] @ x
                alpha[e] = float(p @ scaled[e][m]) / float(p @ p)
            x = solve(blocks(alpha))
    return x, alpha, masks


def metrics(x, alpha, basis0, cgiven, ctrue):
    tot = x.sum()
    fr = np.array([x[w].sum() for w in basis0.win]) / tot
    smean = float(basis0.s @ x) / tot
    totals = [cgiven[e] * alpha[e] * tot / ctrue[e] - 1.0 for e in range(len(cgiven))]
    return {"frac_err": (fr - TRUE_F).tolist(),
            "frac_maxerr": float(np.max(np.abs(fr - TRUE_F))),
            "s_mean_relerr": smean / float(TRUE_S @ TRUE_F) - 1.0,
            "total_relerr": totals}


def per_dataset_sigma(basis, b, ol):
    m = b < ol
    x, rn = nnls(basis.A[m], b[m], maxiter=2000)
    return rn / math.sqrt(m.sum())


# Scenario:  datasets of (rpm, true total OD, sigma, stray, ODlimit,
#  factor applied to the given total concentration)
SCENARIOS = {
    # --- F1: OD limit -----------------------------------------------------
    "X1a_odlimit_stray": [(60000, 2.09, 0.004, 0.005, 1.5, 1.0),
                          (60000, 0.57, 0.004, 0.005, 1.5, 1.0)],
    "X1b_odlimit_linear": [(60000, 2.09, 0.004, 0.0, 1.5, 1.0),
                           (60000, 0.57, 0.004, 0.0, 1.5, 1.0)],
    "X1c_low_odlimit": [(60000, 0.95, 0.004, 0.005, 0.8, 1.0),
                        (60000, 0.38, 0.004, 0.005, 0.8, 1.0)],
    # --- weighting ----------------------------------------------------------
    "X2a_equal_sigma_dilute": [(60000, 0.95, 0.004, 0.0, 1.5, 1.0),
                               (60000, 0.10, 0.004, 0.0, 1.5, 1.0)],
    "X2b_sigma_prop_conc": [(60000, 0.95, 0.004, 0.0, 1.5, 1.0),
                            (60000, 0.10, 0.0004, 0.0, 1.5, 1.0)],
    "X2c_diff_optics": [(60000, 0.95, 0.004, 0.0, 1.5, 1.0),
                        (60000, 0.95, 0.012, 0.0, 1.5, 1.0)],
    "X2d_multispeed": [(60000, 0.95, 0.004, 0.0, 1.5, 1.0),
                       (30000, 0.25, 0.004, 0.0, 1.5, 1.0)],
    # --- concentration misspecification ------------------------------------
    "X3a_conc_err_10pct": [(60000, 0.95, 0.004, 0.0, 1.5, 1.0),
                           (60000, 0.475, 0.004, 0.0, 1.5, 1.10)],
    "X3b_conc_err_multispeed": [(60000, 0.95, 0.004, 0.0, 1.5, 1.0),
                                (30000, 0.475, 0.004, 0.0, 1.5, 0.90)],
}


def run_scenario(name, spec, bases, reps, seed0=12345):
    rng = np.random.default_rng(seed0 + sum(map(ord, name)))
    variants = ["cur", "F1", "W", "What", "Wa"]
    res = {v: [] for v in variants}
    for _ in range(reps):
        dsets, cgiven, ctrue, ols, sig = [], [], [], [], []
        for (rpm, c, sigma, frac, ol, cf) in spec:
            basis = bases[rpm]
            dsets.append((basis, make_data(basis, c, sigma, frac, rng)))
            cgiven.append(c * cf)
            ctrue.append(c)
            ols.append(ol)
            sig.append(sigma)
        sig_hat = [per_dataset_sigma(b, d, ol) for (b, d), ol in zip(dsets, ols)]
        for v in variants:
            se = sig_hat if v == "What" else sig
            x, alpha, _ = fit_variant(v, dsets, cgiven, ols, sig_est=se)
            res[v].append(metrics(x, alpha, dsets[0][0], cgiven, ctrue))
    return summarize(res)


def summarize(res):
    out = {}
    for v, runs in res.items():
        fe = np.array([r["frac_err"] for r in runs])
        se = np.array([r["s_mean_relerr"] for r in runs])
        te = np.array([r["total_relerr"] for r in runs])
        out[v] = {
            "frac_bias": fe.mean(0).tolist(),
            "frac_rmse": np.sqrt((fe ** 2).mean(0)).tolist(),
            "frac_rmse_all": float(np.sqrt((fe ** 2).mean())),
            "s_mean_bias": float(se.mean()),
            "s_mean_rmse": float(np.sqrt((se ** 2).mean())),
            "total_bias": te.mean(0).tolist(),
            "total_rmse": np.sqrt((te ** 2).mean(0)).tolist(),
            "n": len(runs),
        }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("basis_dir")
    ap.add_argument("--reps", type=int, default=100)
    ap.add_argument("--scenarios", default="all")
    ap.add_argument("--out", default="nnls_results.json")
    args = ap.parse_args()
    bases = {rpm: Basis(args.basis_dir, rpm) for rpm in (60000, 30000)}
    names = list(SCENARIOS) if args.scenarios == "all" else args.scenarios.split(",")
    allres = {}
    for name in names:
        allres[name] = run_scenario(name, SCENARIOS[name], bases, args.reps)
        r = allres[name]
        print("%-26s" % name + "  ".join(
            "%s: fr %.4f s %+.4f tot %s" % (
                v, r[v]["frac_rmse_all"], r[v]["s_mean_bias"],
                ",".join("%+.3f" % t for t in r[v]["total_bias"]))
            for v in r), flush=True)
    Path(args.out).write_text(json.dumps(allres, indent=1))


if __name__ == "__main__":
    sys.exit(main())
