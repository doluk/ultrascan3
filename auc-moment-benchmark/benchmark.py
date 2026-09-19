"""
Layer 3 -- write the handoff package.

Produces, for every model that has a usable observable window:

    data/<model_id>/
        y_true.npy        ground-truth moments      y_k = sum_i c_i s_i^k
        y_hat.npy         mean estimated moments
        Sigma.npy         FULL covariance of y-hat, (N x N)
        realizations.npy  every Monte Carlo sample, for independent analysis
        truth.json        atoms, support bounds, R, N_eff, conditioning

Run:  python3 benchmark.py [--n-mc 500] [--m-max 14] [--out data]
"""
import argparse, json, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "forward"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "analysis"))

import numpy as np

import models
from forward_map import hankel
from windowing import observable_window
from phaseD import monte_carlo, n_eff, correlation

SV = 1e-13

# Each case is (model_id, optics preset, TI handling).  All three are shipped
# because the answer depends entirely on which you assume -- see FINDINGS.md.
CASES = [
    ("white-only", "none"),        # the proposal's implicit assumption
    ("absorbance", "none"),        # realistic, favourable optics
    ("interference", "none"),      # realistic, interference optics
    ("interference", "oracle"),    # upper bound on any TI-removal scheme
]


def _fmt(v):
    return "n/a" if v is None else f"{v:.2e}"


def rescale(y, model):
    """
    Express moments with s in units of the support midpoint, y~_k = y_k/s_c^k.

    Without this the Hankel condition number just reports the s^m dynamic
    range (1e38 for s ~ 15 S and m up to 14) rather than anything about how
    hard the inverse problem is.
    """
    a, b = models.support_bounds(model)
    s_c = 0.5 * (a + b)
    return np.asarray(y, float) / s_c ** np.arange(len(y))


def hankel_conditioning(y, model, R):
    """
    Conditioning of the noise-free Hankel matrix, on rescaled moments.

    Two numbers, because they answer different questions:
      full_spectrum -- normalised singular values of the whole Hankel matrix;
                       for an exactly atomic truth these drop to round-off
                       past index R, which is the rank certificate in the
                       noise-free limit
      cond_RxR      -- condition number of the leading R x R block, which is
                       finite and is the number that actually governs how
                       well R atoms can be separated
    """
    yr = rescale(y, model)
    H = hankel(yr)
    sv = np.linalg.svd(H, compute_uv=False)
    out = dict(full_spectrum=[float(v) for v in sv / sv[0]])
    if R is not None and 1 <= R <= H.shape[0]:
        svR = np.linalg.svd(H[:R, :R], compute_uv=False)
        out["cond_RxR"] = float(svR[0] / svR[-1]) if svR[-1] > 0 else float("inf")
    else:
        out["cond_RxR"] = None
    return out


def write_case(outdir, mid, model, geom, res, tag):
    d = os.path.join(outdir, f"{mid}__{tag}")
    os.makedirs(d, exist_ok=True)
    np.save(os.path.join(d, "y_true.npy"), res["y_true"])
    np.save(os.path.join(d, "y_hat.npy"), res["y_hat"])
    np.save(os.path.join(d, "Sigma.npy"), res["Sigma"])
    np.save(os.path.join(d, "realizations.npy"), res["realizations"])

    N, rel = n_eff(res)
    atoms = [dict(s_svedberg=c["s"] / SV, D_cm2_s=c["D"], conc=c["c"],
                  f_f0=c["f_f0"], mw_Da=c["mw"])
             for c in model["components"] if c["c"] > 0]
    R = len(atoms) if model["atomic"] else None
    cond = hankel_conditioning(res["y_true"], model, R)
    lo, hi = models.support_bounds(model)
    corr = correlation(res["Sigma"])
    off = corr - np.eye(corr.shape[0])

    meta = dict(
        model_id=mid, case=tag, note=model["note"],
        atomic=bool(model["atomic"]),
        R=R,
        atoms=atoms,
        support_svedberg=[float(lo), float(hi)],
        rpm=geom.rpm, meniscus=geom.meniscus, bottom=geom.bottom,
        n_scans_total=int(geom.n_scans),
        n_scans_usable=int(res["n_scans_used"]),
        optics=res["preset"], ti_handling=res["ti_mode"],
        n_moments=int(res["y_true"].size),
        N_eff=int(N), R_max=int((N - 1) // 2) if N >= 1 else -1,
        rel_error_by_order=[float(v) for v in rel],
        rel_bias_by_order=[float(b / t) if t != 0 else None
                           for b, t in zip(res["bias"], res["y_true"])],
        hankel_cond_RxR_rescaled=cond["cond_RxR"],
        hankel_spectrum_rescaled=cond["full_spectrum"],
        max_abs_offdiagonal_correlation=float(np.abs(off).max()),
        median_abs_offdiagonal_correlation=float(
            np.median(np.abs(off[~np.eye(corr.shape[0], dtype=bool)]))),
    )
    with open(os.path.join(d, "truth.json"), "w") as f:
        json.dump(meta, f, indent=2)
    return meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n-mc", type=int, default=500)
    ap.add_argument("--m-max", type=int, default=14)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(
        os.path.abspath(__file__)), "data"))
    ap.add_argument("--rpm", type=int, default=40000)
    args = ap.parse_args()

    geom = models.RunGeometry(rpm=args.rpm)
    ts = models.feasible_test_set()
    os.makedirs(args.out, exist_ok=True)

    summary = []
    for mid, model in ts.items():
        if not observable_window(geom, model):
            print(f"{mid:12s} SKIPPED - observable window is empty")
            summary.append(dict(model_id=mid, skipped="empty observable window",
                                note=model["note"]))
            continue
        clean = models.simulate(model, geom)
        for preset, ti_mode in CASES:
            res = monte_carlo(model, geom, m_max=args.m_max, n_mc=args.n_mc,
                              preset=preset, ti_mode=ti_mode, clean=clean)
            tag = f"{preset}-{ti_mode}"
            meta = write_case(args.out, mid, model, geom, res, tag)
            summary.append(meta)
            print(f"{mid:12s} {tag:22s} scans={meta['n_scans_usable']:3d} "
                  f"N_eff={meta['N_eff']:3d} R_max={meta['R_max']:2d} "
                  f"cond(H_RxR)={_fmt(meta['hankel_cond_RxR_rescaled'])} "
                  f"max|corr_offdiag|={meta['max_abs_offdiagonal_correlation']:.3f}")

    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print(f"\nwrote {args.out}/summary.json")


if __name__ == "__main__":
    main()
