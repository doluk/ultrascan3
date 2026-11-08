#!/usr/bin/env python3
"""Summary figure of the 2D study: apparent Delta D / D and Delta s / s
(ideal-model fit, relative to the aligned run on the same grid) versus c0."""
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

R = {r["file"]: r for r in json.load(open("results2d.json"))}
ref = R["runs/p0_c1e-2_120.npz"]
pts = [("runs/p1_c1e-3_120.npz", 1e-3), ("runs/p1_c1e-2_120.npz", 1e-2),
       ("runs/p1_c1e-1_120.npz", 1e-1)]
c = np.array([p[1] for p in pts])
dD = np.array([100 * (R[f]["D_k0"] / ref["D_k0"] - 1) for f, _ in pts])
ds = np.array([100 * (R[f]["s_k0"] / ref["s_k0"] - 1) for f, _ in pts])
nf = R["runs/p1_noflow_120.npz"]
fig, ax = plt.subplots(1, 2, figsize=(10, 3.6))
for a, y, lab, y0 in [(ax[0], dD, "ΔD/D (%)", 100 * (nf["D_k0"] / ref["D_k0"] - 1)),
                      (ax[1], ds, "Δs/s (%)", 100 * (nf["s_k0"] / ref["s_k0"] - 1))]:
    a.semilogx(c, y, "o-", label="ψ = 1°, 2D Stokes–transport")
    a.axhline(y0, ls=":", color="gray", label="ψ = 1°, no flow")
    k = np.polyfit(np.log10(c), np.log10(y), 1)
    cc = np.logspace(-3, 0, 20)
    a.semilogx(cc, 10 ** np.polyval(k, np.log10(cc)), "--", lw=0.8,
               label=f"power law, exponent {k[0]:.2f} (extrapolated)")
    a.set_xlabel("c0 (mg/mL)"); a.set_ylabel(lab); a.legend(fontsize=7)
plt.tight_layout(); plt.savefig("fig2d_summary.png", dpi=110)
print("exponents:", np.polyfit(np.log10(c), np.log10(dD), 1)[0], np.polyfit(np.log10(c), np.log10(ds), 1)[0])
print("extrapolated to 1 mg/mL: dD=%.2f%% ds=%.3f%%" % (10**np.polyval(np.polyfit(np.log10(c), np.log10(dD), 1), 0),
                                                       10**np.polyval(np.polyfit(np.log10(c), np.log10(ds), 1), 0)))
