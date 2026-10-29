#!/usr/bin/env python3
"""GA multi-dataset fitness (ga_worker.cpp get_fitness) with known truth.

Random 3-solute candidates near the true species are scored, as a GA gene
is, by a global NNLS over their columns on the normalized data.  Fitness
formulas compared (lower is better):

  cur     mean_e var_e / maxod_e        (ga_worker.cpp:473, current)
  dim     mean_e var_e / maxod_e^2      (dimensionally consistent)
  nnls    sum_e n_e var_e / sum_e n_e   (objective the NNLS minimizes)
  ml      sum_e n_e var_e c_e^2/sigma_e^2  (likelihood; oracle sigma)

maxod_e as us_mpi_analysis computes it:  max of the normalized data, capped
by the OD limit (current: unscaled limit; F1: limit scaled by 1/c_e).

Quality of a candidate:  signal-weighted relative parameter error
sum_i f_i (|ds_i|/s_i + |dk_i|/k_i) of its fitted solutes against the
nearest true species.  Reported:  pairwise concordance of each formula with
the quality (fraction of candidate pairs ordered the same way), and the
quality of the candidate each formula selects as best in a population.

Usage:  ga_fitness_study.py <basis-dir> [--pops N] [--popsize M]
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from scipy.optimize import nnls

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nnls_study as N  # noqa: E402

SCEN = {
    # equal sigma, one dilute dataset
    "G1_dilute": [(60000, 0.95, 0.004, 0.0, 1.5), (60000, 0.10, 0.004, 0.0, 1.5)],
    # concentrated dataset above the OD limit:  F1 makes maxod differ
    "G2_odlimit": [(60000, 2.09, 0.004, 0.0, 1.5), (60000, 0.57, 0.004, 0.0, 1.5)],
    # different speeds and concentrations
    "G3_multispeed": [(60000, 0.95, 0.004, 0.0, 1.5), (30000, 0.25, 0.004, 0.0, 1.5)],
}


def candidate(basis, rng):
    cols = []
    for s, k in zip(N.TRUE_S, N.TRUE_K):
        ok = np.where((np.abs(basis.s - s) <= 1.5) & (np.abs(basis.k - k) <= 0.5))[0]
        cols.append(int(rng.choice(ok)))
    return sorted(set(cols))


def quality(basis, cols, x):
    tot = x.sum()
    if tot <= 0:
        return 10.0
    err = 0.0
    for c, xv in zip(cols, x):
        ii = int(np.argmin(np.abs(N.TRUE_S - basis.s[c])))
        err += xv / tot * (abs(basis.s[c] - N.TRUE_S[ii]) / N.TRUE_S[ii]
                           + abs(basis.k[c] - N.TRUE_K[ii]) / N.TRUE_K[ii])
    return err


def concordance(f, q):
    f = np.asarray(f)
    q = np.asarray(q)
    df = np.sign(f[:, None] - f[None, :])
    dq = np.sign(q[:, None] - q[None, :])
    m = (df != 0) & (dq != 0)
    return float((df[m] == dq[m]).mean())


def run(name, spec, bases, pops, popsize, rng):
    conc = {k: [] for k in ("cur", "cur_F1", "dim_F1", "nnls", "ml")}
    best = {k: [] for k in conc}
    maxods_cur, maxods_f1 = [], []
    for _ in range(pops):
        data = []
        for (rpm, c, sig, fr, ol) in spec:
            b = N.make_data(bases[rpm], c, sig, fr, rng)
            data.append((bases[rpm], b / c, c, sig, ol))
        mo_cur = [min(bs.max(), ol) for (_, bs, c, _, ol) in data]
        mo_f1 = [min(bs.max(), ol / c) for (_, bs, c, _, ol) in data]
        maxods_cur.append(mo_cur)
        maxods_f1.append(mo_f1)
        fits = {k: [] for k in conc}
        quals = []
        basis0 = data[0][0]
        for _ in range(popsize):
            cols = candidate(basis0, rng)
            A = np.vstack([d[0].A[:, cols] for d in data])
            bb = np.concatenate([d[1] for d in data])
            x, _ = nnls(A, bb)
            var = []
            n = []
            for d in data:
                r = d[1] - d[0].A[:, cols] @ x
                var.append(float(r @ r) / len(r))
                n.append(len(r))
            var = np.array(var)
            n = np.array(n)
            cs = np.array([d[2] for d in data])
            sg = np.array([d[3] for d in data])
            fits["cur"].append(np.mean(var / np.array(mo_cur)))
            fits["cur_F1"].append(np.mean(var / np.array(mo_f1)))
            fits["dim_F1"].append(np.mean(var / np.array(mo_f1) ** 2))
            fits["nnls"].append(float((n * var).sum() / n.sum()))
            fits["ml"].append(float((n * var * cs ** 2 / sg ** 2).sum()))
            quals.append(quality(basis0, cols, x))
        for k in conc:
            conc[k].append(concordance(fits[k], quals))
            best[k].append(quals[int(np.argmin(fits[k]))])
    return {
        "maxod_cur": np.mean(maxods_cur, 0).tolist(),
        "maxod_F1": np.mean(maxods_f1, 0).tolist(),
        "concordance": {k: float(np.mean(v)) for k, v in conc.items()},
        "best_quality": {k: float(np.mean(v)) for k, v in best.items()},
        "oracle_best_quality": None,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("basis_dir")
    ap.add_argument("--pops", type=int, default=20)
    ap.add_argument("--popsize", type=int, default=60)
    ap.add_argument("--out", default="ga_fitness_results.json")
    args = ap.parse_args()
    bases = {rpm: N.Basis(args.basis_dir, rpm) for rpm in (60000, 30000)}
    rng = np.random.default_rng(2024)
    res = {}
    for name, spec in SCEN.items():
        res[name] = run(name, spec, bases, args.pops, args.popsize, rng)
        r = res[name]
        print(name, "maxod cur", np.round(r["maxod_cur"], 3), "F1",
              np.round(r["maxod_F1"], 3))
        print("   concordance", {k: round(v, 4) for k, v in r["concordance"].items()})
        print("   best quality", {k: round(v, 4) for k, v in r["best_quality"].items()},
              flush=True)
    Path(args.out).write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
