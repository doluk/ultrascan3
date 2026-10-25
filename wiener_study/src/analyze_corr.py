import json, sys, os, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

OUT, FIG, REF = sys.argv[1], sys.argv[2], sys.argv[3]   # corr output, figure dir, uncorrected study output
os.makedirs(FIG, exist_ok=True)
CASES = ["IgG10_12mm", "IgG46_1mm", "IgG50_1mm", "BSA140_1mm"]
NA = 6.02214076e23; R = 8.314462618e7; T = 293.15; ETA = 0.01002; RHO = 0.998234

def mw_of(s, k, v):
    return (s * 1e-13 * k * 6 * np.pi * ETA * NA * (3 * v / (4 * np.pi * NA)) ** (1 / 3) / (1 - v * RHO)) ** 1.5

def stats(sol, truth, v=0.73):
    sol = np.array(sol); s, k, c = sol[:, 0], sol[:, 1], sol[:, 2]
    ts = np.array([t["s"] for t in truth])
    edges = np.concatenate([[-np.inf], (ts[1:] + ts[:-1]) / 2, [np.inf]])
    out = []
    for i in range(len(truth)):
        m = (s > edges[i]) & (s <= edges[i + 1]); ci = c[m].sum()
        if ci <= 0: out.append((0, np.nan, np.nan, np.nan)); continue
        out.append((ci / c.sum(), (s[m] * c[m]).sum() / ci, (k[m] * c[m]).sum() / ci,
                    (mw_of(s[m], k[m], v) * c[m]).sum() / ci))
    return out

L = []; P = lambda s: L.append(s)
fig, axs = plt.subplots(1, 4, figsize=(18, 3.8))
for ci_, cid in enumerate(CASES):
    fn = f"{OUT}/{cid}_corr.json"
    if not os.path.exists(fn): continue
    J = json.load(open(fn)); truth = J["truth"]; ns, np_ = J["ns"], J["np"]
    R0 = json.load(open(f"{REF}/{cid}.json"))
    nit = max(f["iter"] for f in J["fits"])
    P(f"\n## {cid}: {J['desc']}  (kappa' = {J['kappa']:.2e} cm^2/fringe, known)")
    # convergence
    ax = axs[ci_]
    for cond, col in (("none", "k"), ("TIRI_only", "b"), ("r0.005", "c"), ("r0.01", "g"), ("r0.02", "r")):
        it = [(f["iter"], f["rmsd"]) for f in J["fits"] if f["cond"] == cond and f["seed"] == 0]
        if it:
            it = np.array(sorted(it)); ax.semilogy(it[:, 0], it[:, 1], "o-", color=col, ms=3, label=cond)
    for f in R0["fits"]:
        if f["skew"] == 0 and f["seed"] == 0 and f["cond"] in ("none", "r0.01"):
            ax.axhline(f["rmsd"], color="k" if f["cond"] == "none" else "g", ls=":", lw=0.8)
    ax.set(title=cid, xlabel="iteration (0 = uncorrected)", ylabel="RMSD (fringes)")
    if ci_ == 0: ax.legend(fontsize=7)
    hdr = "| cond | fit | RMSD | " + " | ".join(f"{t['name']} frac / s / f/f0 / MW (kDa)" for t in truth) + " |"
    P(f"truth: " + ", ".join(f"{t['name']} {t['c']/J['ctot']:.3f} / {t['s']:.2f} / {t['k']:.2f} / {t['mw']/1e3:.0f}" for t in truth))
    P(hdr); P("|" + "---|" * (hdr.count("|") - 1))
    for cond in ("none", "TIRI_only", "r0.005", "r0.01", "r0.02"):
        rowsets = {}
        # reference: unskewed data fit (from main study)
        rowsets["no skew (reference)"] = [f for f in R0["fits"] if f["cond"] == cond and f["skew"] == 0]
        rowsets["skew, uncorrected"] = [f for f in J["fits"] if f["cond"] == cond and f["iter"] == 0]
        rowsets[f"skew, C1 corrected (it {nit})"] = [f for f in J["fits"] if f["cond"] == cond and f["iter"] == nit]
        for lab, fs in rowsets.items():
            if not fs: continue
            rm = np.array([f["rmsd"] for f in fs])
            st = np.array([stats(f["solutes"], truth) for f in fs])   # nfit x nsp x 4
            cells = []
            for i in range(len(truth)):
                m = np.nanmean(st[:, i, :], axis=0); sd = np.nanstd(st[:, i, :], axis=0, ddof=1) if len(fs) > 1 else np.zeros(4)
                cells.append(f"{m[0]:.3f} / {m[1]:.2f} / {m[2]:.2f} / {m[3]/1e3:.0f}" + (f" (±{sd[3]/1e3:.0f})" if len(fs) > 1 else ""))
            P(f"| {cond} | {lab} | {rm.mean():.4f} | " + " | ".join(cells) + " |")
    # residual maps uncorrected vs corrected (r0.01 seed 0)
    x = np.array(J["x"])
    f2, a2 = plt.subplots(1, 2, figsize=(12, 4))
    rU = np.fromfile(f"{REF}/{cid}_r0.01_sk1_resid.bin").reshape(ns, np_)
    rC = np.fromfile(f"{OUT}/{cid}_r0.01_corr_resid.bin").reshape(ns, np_)
    vm = np.percentile(np.abs(rU), 99.5)
    for a, r, t in ((a2[0], rU, "uncorrected"), (a2[1], rC, f"C1 corrected (iter {nit})")):
        im = a.imshow(r, aspect="auto", cmap="RdBu_r", vmin=-vm, vmax=vm, extent=[x[0], x[-1], ns, 0])
        a.set(title=f"{cid} residuals, 0.01 fringe + TI/RI, {t}: rmsd {np.sqrt((r**2).mean()):.4f}", xlabel="r (cm)", ylabel="scan")
        plt.colorbar(im, ax=a)
    f2.tight_layout(); f2.savefig(f"{FIG}/{cid}_corr_resid.png", dpi=105); plt.close(f2)
fig.tight_layout(); fig.savefig(f"{FIG}/convergence.png", dpi=110)
open(f"{FIG}/summary_corr.md", "w").write("\n".join(L)); print("\n".join(L))
