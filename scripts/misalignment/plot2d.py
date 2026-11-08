#!/usr/bin/env python3
"""Plot a 2D run against the aligned reference: c field at the last scan,
lateral-average profiles and the difference to the aligned cell."""
import sys, json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

fn, ref, png = sys.argv[1], sys.argv[2], sys.argv[3]
d, d0 = np.load(fn), np.load(ref)
a = json.loads(str(d["args"]))
C, C0 = d["C"] / a["c0"], d0["C"] / json.loads(str(d0["args"]))["c0"]
r = d["x"] + float(d["r_m"])
fig = plt.figure(figsize=(12, 7.5))
ax = fig.add_subplot(3, 1, 1)
im = ax.pcolormesh(r, d["y"] * 10, C[-1].T, shading="auto", cmap="viridis")
ax.set_ylabel("y (mm)"); ax.set_title(f"c / c0 at t = {d['t'][-1]:.0f} s  (ψ = {a['psi']}°, c0 = {a['c0']} mg/mL)")
fig.colorbar(im, ax=ax, pad=0.01)
ax = fig.add_subplot(3, 1, 2)
for i in range(1, len(d["t"]), 2):
    ax.plot(r, C0[i].mean(1), "k-", lw=0.8, label="aligned" if i == 1 else None)
    ax.plot(r, C[i].mean(1), "r--", lw=0.8, label="misaligned, lateral mean" if i == 1 else None)
ax.set_ylabel("c̄ / c0"); ax.legend(loc="upper left"); ax.set_ylim(0, 1.6)
ax = fig.add_subplot(3, 1, 3)
for i in range(1, len(d["t"]), 2):
    ax.plot(r, C[i].mean(1) - C0[i].mean(1), lw=0.8, label=f"t={d['t'][i]:.0f}s")
ax.set_ylabel("c̄ − c̄_aligned"); ax.set_xlabel("r (cm)"); ax.legend(fontsize=7, ncol=3)
plt.tight_layout(); plt.savefig(png, dpi=100)
