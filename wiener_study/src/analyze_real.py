import json, sys, os, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

R, FIG = sys.argv[1], sys.argv[2]
os.makedirs(FIG, exist_ok=True)
NA = 6.02214076e23; ETA = 0.01002; RHO = 0.998234
def mw_of(s, k, v=0.73):
    return (s * 1e-13 * k * 6 * np.pi * ETA * NA * (3 * v / (4 * np.pi * NA)) ** (1 / 3) / (1 - v * RHO)) ** 1.5

# peak windows (S) and paper values (Chaturvedi & Schuck 2024, c_HS(s0) / c(s) analyses)
SETS = {
    "IgG10_12mm": dict(win=[("monomer", 5.0, 8.0), ("dimer", 8.0, 10.8)], title="hIgG 10 mg/ml, 12 mm",
                       paper="c_HS(s0): monomer 6.44 S, 74.1 %; dimer 9.3 S, 15.8 %"),
    "IgG46_1mm": dict(win=[("monomer", 5.0, 8.0), ("dimer", 8.0, 10.8)], title="hIgG 46 mg/ml, 1 mm",
                      paper="c_HS(s0): monomer 6.73 S, 71.1 %; dimer 9.4 S, 16.5 %"),
    "BSA140_1mm": dict(win=[("monomer", 2.5, 5.2), ("dimer", 5.2, 7.6)], title="BSA 140 mg/ml, 1 mm",
                       paper="c_HS(s0): monomer 3.9 S; dimer 22 %; rmsd 0.175 fringe (k_S,red 0.66)"),
    "BSA05_dilute": dict(win=[("monomer", 2.5, 5.2), ("dimer", 5.2, 7.6)], title="BSA 0.5 mg/ml (control)",
                         paper="c(s): monomer 4.2 S; dimer 19 %; f/f0 1.43"),
}
L = []; P = L.append
P("| data set | fit | RMSD (fringe) | max Δr (µm) | " + "species: frac / s (S) / f/f0 / MW (kDa) | " * 2 + "paper |")
P("|---|---|---|---|---|---|---|")
fig, axs = plt.subplots(len(SETS), 3, figsize=(19, 4.2 * len(SETS)))
for row, (sid, cfg) in enumerate(SETS.items()):
    fn = f"{R}/{sid}_real.json"
    if not os.path.exists(fn):
        continue
    J = json.load(open(fn)); ns, np_ = J["ns"], J["np"]; x = np.array(J["x"]); t = np.array(J["t"])
    fits = J["fits"]; last = max(f["iter"] for f in fits)
    sel = [(0, "2DSA uncorrected")] + ([(last, f"2DSA + C1 skew (it {last})")] if last > 0 else [])
    for it, lab in sel:
        f = [g for g in fits if g["iter"] == it][0]
        sol = np.array(f["solutes"]); s, k, c = sol[:, 0], sol[:, 1], sol[:, 2]
        cells = []
        for name, a, b in cfg["win"]:
            m = (s >= a) & (s < b); ci = c[m].sum()
            cells.append(f"{name} {ci / c.sum():.3f} / {(s[m]*c[m]).sum()/ci:.2f} / {(k[m]*c[m]).sum()/ci:.2f} / "
                         f"{(mw_of(s[m], k[m])*c[m]).sum()/ci/1e3:.0f}")
        P(f"| {cfg['title']} | {lab} | {f['rmsd']:.4f} | {f['maxdr']*1e4:.0f} | " + " | ".join(cells) + f" | {cfg['paper'] if it == 0 else ''} |")
    # s distribution
    a = axs[row, 0]
    for (it, lab), col in zip(sel, ("k", "r")):
        f = [g for g in fits if g["iter"] == it][0]; sol = np.array(f["solutes"])
        ss = np.linspace(0, 14, 1400); g = sum(cc * np.exp(-0.5 * ((ss - s0) / 0.08) ** 2) for s0, k0, cc in sol)
        a.plot(ss, g / (sol[:, 2].sum()), col, label=f"{lab} (rmsd {f['rmsd']:.4f})")
    a.set(title=f"{cfg['title']}: s distribution (smoothed, normalised)", xlabel="s (S)"); a.legend(fontsize=8)
    # residual maps
    for j, (it, lab) in enumerate(sel):
        r = np.fromfile(f"{R}/{sid}_it{it}_resid.bin").reshape(ns, np_)
        vm = np.percentile(np.abs(np.fromfile(f"{R}/{sid}_it0_resid.bin")), 99)
        im = axs[row, 1 + j].imshow(r, aspect="auto", cmap="RdBu_r", vmin=-vm, vmax=vm, extent=[x[0], x[-1], ns, 0])
        axs[row, 1 + j].set(title=f"residuals, {lab}", xlabel="r (cm)", ylabel="scan")
        plt.colorbar(im, ax=axs[row, 1 + j])
    if len(sel) == 1:
        axs[row, 2].axis("off")
fig.tight_layout(); fig.savefig(f"{FIG}/real_data.png", dpi=95)

# overlay of data and fits for selected scans
fig, axs = plt.subplots(1, len(SETS), figsize=(22, 4.5))
for j, sid in enumerate(SETS):
    fn = f"{R}/{sid}_real.json"
    if not os.path.exists(fn): continue
    J = json.load(open(fn)); ns, np_ = J["ns"], J["np"]; x = np.array(J["x"])
    last = max(f["iter"] for f in J["fits"])
    d = np.fromfile(f"{R}/{sid}_data.bin").reshape(ns, np_)
    f0 = np.fromfile(f"{R}/{sid}_it0_fit.bin").reshape(ns, np_)
    f1 = np.fromfile(f"{R}/{sid}_it{last}_fit.bin").reshape(ns, np_)
    for i in range(0, ns, max(1, ns // 8)):
        axs[j].plot(x, d[i], ".", ms=1, color="0.6")
        axs[j].plot(x, f0[i], "k", lw=0.7)
        if last: axs[j].plot(x, f1[i], "r", lw=0.7)
    axs[j].set(title=f"{SETS[sid]['title']}: data (grey), uncorrected (black), C1 (red)", xlabel="r (cm)")
fig.tight_layout(); fig.savefig(f"{FIG}/real_fits.png", dpi=95)
open(f"{FIG}/summary_real.md", "w").write("\n".join(L)); print("\n".join(L))
