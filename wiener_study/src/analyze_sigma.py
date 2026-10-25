import json, sys, os, glob, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

R, FIG = sys.argv[1], sys.argv[2]
os.makedirs(FIG, exist_ok=True)
NA = 6.02214076e23; ETA = 0.01002; RHO = 0.998234
def mw_of(s, k, v=0.73):
    return (s * 1e-13 * k * 6 * np.pi * ETA * NA * (3 * v / (4 * np.pi * NA)) ** (1 / 3) / (1 - v * RHO)) ** 1.5

SETS = {
    "IgG10_12mm": dict(win=[("monomer", 5.0, 8.0), ("dimer", 8.0, 10.8)], title="hIgG 10 mg/ml, 12 mm",
                       paper="6.44 S 74.1 %; 9.3 S 15.8 %; rmsd 0.041"),
    "IgG46_1mm": dict(win=[("monomer", 5.0, 8.0), ("dimer", 8.0, 10.8)], title="hIgG 46 mg/ml, 1 mm",
                      paper="6.73 S 71.1 %; 9.4 S 16.5 %"),
    "BSA140_1mm_b660": dict(win=[("monomer", 2.5, 5.2), ("dimer", 5.2, 7.6)], title="BSA 140 mg/ml, 1 mm",
                            paper="3.9 S; dimer 22 %; rmsd 0.175"),
}

def stats(sol, win):
    sol = np.array(sol); s, k, c = sol[:, 0], sol[:, 1], sol[:, 2]; out = []
    for name, a, b in win:
        m = (s >= a) & (s < b); ci = c[m].sum()
        if ci <= 0: out.append(f"{name} 0"); continue
        out.append(f"{name} {ci/c.sum():.3f} / {(s[m]*c[m]).sum()/ci:.2f} / {(k[m]*c[m]).sum()/ci:.2f} / "
                   f"{(mw_of(s[m], k[m])*c[m]).sum()/ci/1e3:.0f}")
    return out

L = []; P = L.append
fig, axs = plt.subplots(1, len(SETS), figsize=(6 * len(SETS), 4))
best = {}
for j, (sid, cfg) in enumerate(SETS.items()):
    runs = []
    for fn in glob.glob(f"{R}/{sid}_s*_real.json"):
        J = json.load(open(fn)); fits = J["fits"]; last = max(f["iter"] for f in fits)
        f0 = [f for f in fits if f["iter"] == 0][0]; f1 = [f for f in fits if f["iter"] == last][0]
        runs.append((J["sigma"], f0, f1, J, fn))
    if not runs: continue
    runs.sort(key=lambda r: r[0])
    sg = [r[0] for r in runs]
    axs[j].plot(sg, [r[1]["rmsd"] for r in runs], "ko-", label="ASTFVM σ, no skew")
    axs[j].plot(sg, [r[2]["rmsd"] for r in runs], "ro-", label="ASTFVM σ + C1 skew")
    axs[j].set(title=cfg["title"], xlabel="σ (dimensionless; basis at unit signal)", ylabel="RMSD (fringes)")
    axs[j].legend(fontsize=8)
    P(f"\n## {cfg['title']}  (paper: {cfg['paper']})")
    P("| σ | RMSD no skew | RMSD + C1 | species (no skew): frac / s / f/f0 / MW | species (+C1): frac / s / f/f0 / MW |")
    P("|---|---|---|---|---|")
    for sgm, f0, f1, J, fn in runs:
        P(f"| {sgm:g} | {f0['rmsd']:.4f} | {f1['rmsd']:.4f} | " + "; ".join(stats(f0["solutes"], cfg["win"])) +
          " | " + "; ".join(stats(f1["solutes"], cfg["win"])) + " |")
    b0 = min(runs, key=lambda r: r[1]["rmsd"]); b1 = min(runs, key=lambda r: r[2]["rmsd"])
    P(f"best σ without skew: {b0[0]:g} (rmsd {b0[1]['rmsd']:.4f}); best σ with C1: {b1[0]:g} (rmsd {b1[2]['rmsd']:.4f})")
    best[sid] = (b0, b1)
fig.tight_layout(); fig.savefig(f"{FIG}/sigma_scan.png", dpi=100)

# residual maps + s distributions at best sigma
fig, axs = plt.subplots(len(best), 3, figsize=(19, 4.2 * len(best)))
axs = np.atleast_2d(axs)
for row, (sid, (b0, b1)) in enumerate(best.items()):
    for (sgm, f0, f1, J, fn), it, col, lab, ax in ((b0, 0, "k", "no skew", axs[row, 1]), (b1, None, "r", "+ C1", axs[row, 2])):
        ns, np_ = J["ns"], J["np"]; x = np.array(J["x"]); last = max(f["iter"] for f in J["fits"])
        itx = 0 if it == 0 else last
        stem = fn.replace("_real.json", "")
        r = np.fromfile(f"{stem}_it{itx}_resid.bin").reshape(ns, np_)
        vm = np.percentile(np.abs(r), 99)
        im = ax.imshow(r, aspect="auto", cmap="RdBu_r", vmin=-vm, vmax=vm, extent=[x[0], x[-1], ns, 0])
        ax.set(title=f"{SETS[sid]['title']}: residuals σ={sgm:g} {lab} (rmsd {np.sqrt((r**2).mean()):.4f})", xlabel="r (cm)")
        plt.colorbar(im, ax=ax)
        f = f0 if it == 0 else f1
        sol = np.array(f["solutes"]); ss = np.linspace(0, 14, 1400)
        g = sum(cc * np.exp(-0.5 * ((ss - s0) / 0.1) ** 2) for s0, k0, cc in sol)
        axs[row, 0].plot(ss, g / sol[:, 2].sum(), col, label=f"σ={sgm:g} {lab}")
    axs[row, 0].set(title=f"{SETS[sid]['title']}: s distribution at best σ", xlabel="s (S)"); axs[row, 0].legend(fontsize=8)
fig.tight_layout(); fig.savefig(f"{FIG}/sigma_best.png", dpi=95)
open(f"{FIG}/summary_sigma.md", "w").write("\n".join(L)); print("\n".join(L))
