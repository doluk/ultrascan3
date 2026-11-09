#!/usr/bin/env python3
"""Summary figure of the 2D study: apparent Delta D / D and Delta s / s
(ideal-model fit, relative to the aligned run on the same grid) versus c0.
Main series: BDF2 solver, wall-clustered 120x40 grid (misalign2d_bdf2.py);
grey: first-order backward-Euler solver on a uniform grid (misalign2d_implicit.py)."""
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

R = {r["file"]: r for r in json.load(open("results2d.json"))}


def rel(files, ref, key):
    return np.array([100 * (R[f][key] / R[ref][key] - 1) for f in files])


bdf = [("runs/b_p1_c1e-2_120_y3.npz", 1e-2), ("runs/b_p1_c0.1_y3.npz", 1e-1),
       ("runs/b_p1_c0.3_y3.npz", 0.3), ("runs/b_p1_c1.0_y3.npz", 1.0)]
old = [("runs/p1_c1e-3_120.npz", 1e-3), ("runs/p1_c1e-2_120.npz", 1e-2),
       ("runs/p1_c1e-1_120.npz", 1e-1)]
c = np.array([p[1] for p in bdf]); co = np.array([p[1] for p in old])
fb = [p[0] for p in bdf]; fo = [p[0] for p in old]
p2 = "runs/b_p2_c0.1_y3.npz"

fig, ax = plt.subplots(1, 2, figsize=(10, 3.6))
for a, key, lab in [(ax[0], "D_k0", "ΔD/D (%)"), (ax[1], "s_k0", "Δs/s (%)")]:
    y = rel(fb, "runs/b_p0_120.npz", key)
    a.semilogx(c, y, "o-", label="ψ = 1°, BDF2, clustered grid")
    a.semilogx([0.1], rel([p2], "runs/b_p0_120.npz", key) / 2, "s", ms=7, mfc="none",
               label="ψ = 2° (value / 2)")
    a.semilogx(co, rel(fo, "runs/p0_c1e-2_120.npz", key), "o:", color="0.6",
               label="ψ = 1°, backward Euler, uniform grid")
    k = np.polyfit(np.log10(c), np.log10(y), 1)
    cc = np.logspace(-2, 0, 20)
    a.semilogx(cc, 10 ** np.polyval(k, np.log10(cc)), "--", lw=0.8,
               label=f"power law, exponent {k[0]:.2f}")
    a.set_ylim(bottom=0)
    a.set_xlabel("c0 (mg/mL)"); a.set_ylabel(lab); a.legend(fontsize=7)
    print(key, "exponent", round(k[0], 3), "values", np.round(y, 3))
plt.tight_layout(); plt.savefig("fig2d_summary.png", dpi=110)
