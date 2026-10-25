import json, sys, os, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

# usage: analyze_mf.py FIGDIR run1.json[:label] run2.json[:label] ...
FIG = sys.argv[1]; os.makedirs(FIG, exist_ok=True)
WIN = {"IgG": [("fragment", 2, 5), ("monomer", 5, 8), ("dimer", 8, 10.8), ("larger", 10.8, 20)],
       "BSA": [("monomer", 2, 5.2), ("dimer", 5.2, 7.6), ("larger", 7.6, 15)],
       "F4HS": [("below", 1, 6), ("main", 6, 7), ("above", 7, 20)]}
PAPER = {"IgG10": "c_HS(s0): monomer 74.1 % @ 6.44 S; dimer 15.8 % @ 9.3 S; rmsd 0.041",
         "IgG46": "c_HS(s0): monomer 71.1 % @ 6.73 S; dimer 16.5 % @ 9.4 S",
         "BSA140": "c_HS(s0): monomer 3.9 S; dimer 22 %; rmsd 0.175",
         "F4HS": "truth: single species 6.5 S"}
L = []; P = L.append
P("| run | fit | iter | RMSD | c_tot | composition: fraction @ s (S) | reference |")
P("|---|---|---|---|---|---|---|")
runs = []
for arg in sys.argv[2:]:
    fn, _, lab = arg.partition(":")
    J = json.load(open(fn)); stem = fn.replace("_real.json", "")
    key = next(k for k in WIN if k in os.path.basename(fn))
    pk = next((k for k in PAPER if os.path.basename(fn).startswith(k)), "")
    last = max(f["iter"] for f in J["fits"])
    for f in J["fits"]:
        if f["iter"] not in (0, last): continue
        sol = np.array(f["solutes"]); s, c = sol[:, 0], sol[:, 2]; tot = c.sum(); cells = []
        for n, a, b in WIN[key]:
            m = (s >= a) & (s < b); ci = c[m].sum()
            cells.append(f"{n} {100*ci/tot:.1f} % @ {(s[m]*c[m]).sum()/ci:.2f}" if ci > 0 else f"{n} 0")
        kind = f.get("kind", "")
        P(f"| {lab or os.path.basename(stem)} | {'ideal 1D (f/f0 fixed)' if kind == 'ideal' else 'mean field' + (' + C1 skew' if J['kappa'] else '')} | {f['iter']} | "
          f"{f['rmsd']:.4f} | {tot:.2f} | " + "; ".join(cells) + f" | {PAPER.get(pk, '') if kind != 'ideal' else ''} |")
    runs.append((lab or os.path.basename(stem), J, stem, last))
# figure: s distributions + residual maps
fig, axs = plt.subplots(len(runs), 3, figsize=(19, 4 * len(runs)))
axs = np.atleast_2d(axs)
for row, (lab, J, stem, last) in enumerate(runs):
    ns, np_ = J["ns"], J["np"]; x = np.array(J["x"])
    for f, col in ((J["fits"][0], "k"), ([g for g in J["fits"] if g["iter"] == last][0], "r")):
        sol = np.array(f["solutes"]); ss = np.linspace(0, 16, 1600)
        g = sum(cc * np.exp(-0.5 * ((ss - s0) / 0.1) ** 2) for s0, k0, cc in sol)
        axs[row, 0].plot(ss, g / sol[:, 2].sum(), col, label=f"{f.get('kind','')} it {f['iter']} rmsd {f['rmsd']:.4f}")
    axs[row, 0].set(title=f"{lab}: s distribution (smoothed, normalised)", xlabel="s (S)"); axs[row, 0].legend(fontsize=8)
    for j, tg in enumerate(("ideal", "mf")):
        fn = f"{stem}_{tg}_resid.bin"
        if not os.path.exists(fn): axs[row, 1 + j].axis("off"); continue
        r = np.fromfile(fn).reshape(ns, np_); vm = np.percentile(np.abs(np.fromfile(f"{stem}_mf_resid.bin")), 99) * 2
        im = axs[row, 1 + j].imshow(r, aspect="auto", cmap="RdBu_r", vmin=-vm, vmax=vm, extent=[x[0], x[-1], ns, 0])
        axs[row, 1 + j].set(title=f"{lab}: residuals {tg} (rmsd {np.sqrt((r**2).mean()):.4f})", xlabel="r (cm)", ylabel="scan")
        plt.colorbar(im, ax=axs[row, 1 + j])
fig.tight_layout(); fig.savefig(f"{FIG}/meanfield.png", dpi=90)
# data vs fit overlays
fig, axs = plt.subplots(1, len(runs), figsize=(5.5 * len(runs), 4)); axs = np.atleast_1d(axs)
for j, (lab, J, stem, last) in enumerate(runs):
    ns, np_ = J["ns"], J["np"]; x = np.array(J["x"])
    d = np.fromfile(f"{stem}_data.bin").reshape(ns, np_); fm = np.fromfile(f"{stem}_mf_fit.bin").reshape(ns, np_)
    for k in range(0, ns, max(1, ns // 10)):
        axs[j].plot(x, d[k], ".", ms=1, color="0.6"); axs[j].plot(x, fm[k], "r", lw=0.7)
    axs[j].set(title=f"{lab}: data (grey), mean-field fit (red)", xlabel="r (cm)")
fig.tight_layout(); fig.savefig(f"{FIG}/meanfield_fits.png", dpi=90)
open(f"{FIG}/summary_meanfield.md", "w").write("\n".join(L)); print("\n".join(L))
