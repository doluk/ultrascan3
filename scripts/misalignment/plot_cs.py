#!/usr/bin/env python3
"""c(s) overlay of the multi-species 2D runs (BSA monomer/dimer, Channell
et al. 2018 conditions) for psi = 0, 1, 2, 4 deg; f/f0 fixed and fitted."""
import json, sys
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

runs = [("m_p0", 0), ("m_p1", 1), ("m_p2", 2), ("m_p4", 4)]
fig, ax = plt.subplots(1, 2, figsize=(10, 3.8), sharey=True)
for a, tag, title in [(ax[0], "ff1.25", "f/f0 fixed 1.25"), (ax[1], "fit", "f/f0 fitted")]:
    for r, psi in runs:
        d = json.load(open(f"runs/{r}.cs_{tag}.json"))
        a.plot(d["s"], d["c"], label=f"{psi}°  (f/f0={d['ff0']:.2f}, aggr. {100*(d['f_dimer']+d['f_higher']):.1f}%)")
    a.set_xlim(3, 10); a.set_xlabel("s (S)"); a.set_title(title); a.legend(fontsize=8)
ax[0].set_ylabel("c(s) (fraction / S)")
fig.tight_layout(); fig.savefig("fig_cs_overlay.png", dpi=150)
