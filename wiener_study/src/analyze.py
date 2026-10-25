import json, sys, os, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = sys.argv[1]
FIG = sys.argv[2]
os.makedirs(FIG, exist_ok=True)
CASES = ["IgG10_12mm", "IgG46_1mm", "IgG50_1mm", "BSA140_1mm"]
NA = 6.02214076e23; R = 8.314462618e7; T = 293.15; ETA = 0.01002; RHO = 0.998234

def mw_of(s, k, vbar):
    # s in S, k = f/f0 -> MW (g/mol), standard UltraScan relation
    s = s * 1e-13
    return (s * k * 6 * np.pi * ETA * NA * (3 * vbar / (4 * np.pi * NA)) ** (1 / 3) / (1 - vbar * RHO)) ** 1.5

def D_of(s, mw, vbar):
    return s * 1e-13 * R * T / (mw * (1 - vbar * RHO))

def species_stats(sol, truth, vbar=0.73):
    sol = np.array(sol)
    s, k, c = sol[:, 0], sol[:, 1], sol[:, 2]
    ts = np.array([t["s"] for t in truth])
    # assign solutes to nearest true species (in s), midpoints as borders
    edges = np.concatenate([[-np.inf], (ts[1:] + ts[:-1]) / 2, [np.inf]])
    res = []
    ctot = c.sum()
    for i, t in enumerate(truth):
        m = (s > edges[i]) & (s <= edges[i + 1])
        ci = c[m].sum()
        if ci <= 0:
            res.append(dict(name=t["name"], frac=0, s=np.nan, k=np.nan, mw=np.nan, D=np.nan)); continue
        sm = (s[m] * c[m]).sum() / ci
        km = (k[m] * c[m]).sum() / ci
        mws = mw_of(s[m], k[m], vbar)
        Ds = D_of(s[m], mws, vbar)
        res.append(dict(name=t["name"], frac=ci / ctot, s=sm, k=km,
                        mw=(mws * c[m]).sum() / ci, D=(Ds * c[m]).sum() / ci))
    return res, ctot

rows = []
summary = {}
for cid in CASES:
    fn = f"{OUT}/{cid}.json"
    if not os.path.exists(fn):
        continue
    J = json.load(open(fn))
    truth = J["truth"]; ctot_true = sum(t["c"] for t in truth)
    ns, np_ = J["ns"], J["np"]
    x = np.array(J["x"]); tt = np.array(J["t"])
    summary[cid] = J
    for f in J["fits"]:
        st, ct = species_stats(f["solutes"], truth)
        rows.append(dict(case=cid, cond=f["cond"], seed=f["seed"], skew=f["skew"], rmsd=f["rmsd"],
                         ctot=ct / ctot_true, nsol=len(f["solutes"]), sp=st, sol=f["solutes"],
                         ti=f.get("ti_fit")))

def agg(case, cond, skew, key, sp=None):
    v = []
    for r in rows:
        if r["case"] == case and r["cond"] == cond and r["skew"] == skew:
            v.append(r[key] if sp is None else r["sp"][sp][key])
    v = np.array(v, float)
    return v.mean(), (v.std(ddof=1) if len(v) > 1 else 0.0)

conds = ["none", "TIRI_only", "r0.005", "r0.01", "r0.02"]
lines = []
P = lambda *a: lines.append(" ".join(str(x) for x in a))
for cid, J in summary.items():
    truth = J["truth"]
    P(f"\n## {cid}: {J['desc']}")
    P(f"kappa' = {J['kappa']:.2e} cm^2/fringe, c_tot = {J['ctot']:.2f} fringes, "
      f"max slope (fit range) = {J['maxslope']:.0f} fringes/cm, max radial shift = {J['maxshift_cm']*1e4:.1f} um, "
      f"non-monotonic mapping points = {J['nonmono']}")
    P("truth: " + ", ".join(f"{t['name']} s={t['s']:.2f} f/f0={t['k']:.3f} frac={t['c']/J['ctot']:.3f}" for t in truth))
    hdr = "| cond | skew | RMSD (fringe) | c_tot rel | " + " | ".join(
        f"{t['name']} frac / s / f/f0 / MW(kDa)" for t in truth) + " |"
    P(hdr); P("|" + "---|" * (hdr.count("|") - 1))
    for c in conds:
        for sk in (0, 1):
            rm, rs = agg(cid, c, sk, "rmsd")
            cm, _ = agg(cid, c, sk, "ctot")
            cells = []
            for i, t in enumerate(truth):
                fm, fs = agg(cid, c, sk, "frac", i); sm, ss_ = agg(cid, c, sk, "s", i)
                km, ks = agg(cid, c, sk, "k", i); mm, ms = agg(cid, c, sk, "mw", i)
                cells.append(f"{fm:.3f}±{fs:.3f} / {sm:.3f}±{ss_:.3f} / {km:.2f}±{ks:.2f} / {mm/1e3:.0f}±{ms/1e3:.0f}")
            P(f"| {c} | {sk} | {rm:.5f}±{rs:.5f} | {cm:.4f} | " + " | ".join(cells) + " |")
open(f"{FIG}/summary.md", "w").write("\n".join(lines))
print("\n".join(lines))

# ---------------- figures ----------------
for cid, J in summary.items():
    ns, np_ = J["ns"], J["np"]; x = np.array(J["x"]); truth = J["truth"]
    d0 = np.fromfile(f"{OUT}/{cid}_data_noskew.bin").reshape(ns, np_)
    dd = np.fromfile(f"{OUT}/{cid}_skewdiff.bin").reshape(ns, np_)
    fig, ax = plt.subplots(2, 3, figsize=(16, 8.5))
    sel = range(0, ns, 6)
    for i in sel:
        ax[0, 0].plot(x, d0[i], lw=0.8)
    ax[0, 0].set(title=f"{cid}: simulated signal (no noise, no skew)", xlabel="r (cm)", ylabel="fringes")
    for i in sel:
        ax[0, 1].plot(x, dd[i], lw=0.8)
    ax[0, 1].set(title="skewed − unskewed (incl. TI in object plane)", xlabel="r (cm)", ylabel="Δ fringes")
    ax[0, 2].plot(np.array(J["t"]), np.array(J["shift_per_scan"]) * 1e4, "o-", ms=3)
    ax[0, 2].set(title="max radial displacement per scan", xlabel="t (s)", ylabel="Δr (µm)")
    for j, (cond, ttl) in enumerate([("none", "residuals, no noise"), ("r0.01", "residuals, 0.01 fringe + TI/RI")]):
        r1 = np.fromfile(f"{OUT}/{cid}_{cond}_sk1_resid.bin").reshape(ns, np_)
        r0 = np.fromfile(f"{OUT}/{cid}_{cond}_sk0_resid.bin").reshape(ns, np_)
        vm = np.percentile(np.abs(r1), 99.5)
        im = ax[1, j].imshow(r1, aspect="auto", cmap="RdBu_r", vmin=-vm, vmax=vm,
                             extent=[x[0], x[-1], ns, 0])
        plt.colorbar(im, ax=ax[1, j])
        ax[1, j].set(title=f"{ttl}, WITH skew (rmsd {np.sqrt((r1**2).mean()):.4f}; no skew {np.sqrt((r0**2).mean()):.4f})",
                     xlabel="r (cm)", ylabel="scan")
    # pseudo c(s) distributions
    a = ax[1, 2]
    for sk, col in ((0, "k"), (1, "r")):
        for r in rows:
            if r["case"] == cid and r["cond"] == "r0.01" and r["skew"] == sk and r["seed"] == 0:
                sol = np.array(r["sol"])
                ss = np.linspace(sol[:, 0].min() - 1, sol[:, 0].max() + 1, 600)
                g = sum(c * np.exp(-0.5 * ((ss - s) / 0.08) ** 2) for s, k, c in sol)
                a.plot(ss, g, col, label=f"skew={sk}")
                a.vlines(sol[:, 0], 0, sol[:, 2], color=col, alpha=0.4)
    for t in truth:
        a.axvline(t["s"], color="g", ls=":", lw=0.8)
    a.set(title="2DSA s-distribution (0.01 fringe noise, seed 0)", xlabel="s (S)", ylabel="fringes (smoothed)")
    a.legend()
    fig.tight_layout(); fig.savefig(f"{FIG}/{cid}.png", dpi=110); plt.close(fig)

    # 2D s-f/f0 plots
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
    for sk in (0, 1):
        for r in rows:
            if r["case"] == cid and r["cond"] == "r0.01" and r["skew"] == sk and r["seed"] == 0:
                sol = np.array(r["sol"])
                ax[sk].scatter(sol[:, 0], sol[:, 1], s=8 + 300 * sol[:, 2] / sol[:, 2].max(), alpha=0.6)
        for t in truth:
            ax[sk].plot(t["s"], t["k"], "rx", ms=10)
        ax[sk].set(title=f"{cid} 2DSA s vs f/f0, skew={sk} (0.01 fringe)", xlabel="s (S)", ylabel="f/f0")
    fig.tight_layout(); fig.savefig(f"{FIG}/{cid}_2d.png", dpi=110); plt.close(fig)

json.dump([{k: v for k, v in r.items() if k not in ("sol", "ti")} for r in rows],
          open(f"{FIG}/rows.json", "w"), default=float, indent=0)
