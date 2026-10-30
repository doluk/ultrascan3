#!/usr/bin/env python3
"""Summarize gfit_e2e.py results:  markdown tables and a figure.

Per scenario and variant (mean over seeds), for SC (global solutes scaled to
each dataset) and VR (global solutes refit to each dataset):
  frac   max |fraction error| over s windows, mean over datasets and seeds
  s      signal-weighted mean s, relative error (bias)
  tot    total concentration relative error per dataset (bias)
  totR   RMS over datasets and seeds of the total error
  scale  max |fitted scale / true scale - 1|
  sigma  max |estimated sigma / true sigma - 1|
  rmsd   reported RMSD / true sigma, per dataset

Diagnostic table (composition differences between datasets):
  dtot   (total SC - total VR) / total VR per dataset
  dfrac  max |fraction SC - fraction VR| over s windows, per dataset
  grmsd  global RMSD / sigma, pooled over datasets from the SC models (the
         global fit's prediction of each dataset), equal dataset weights

Usage:  gfit_summary.py results.json [more.json] [--fig out.png] [--md out.md]
"""
import argparse
import json
import math
import os
from collections import defaultdict

import numpy as np

VORDER = ["none", "W", "S", "WS"]


def collect(rows):
    g = defaultdict(list)
    for r in rows:
        g[(r["scenario"], r["variant"])].append(r)
    return g


def stats(runs, kind="SC"):
    nds = len(runs[0]["true_scales"])
    fr, sm, tot, rm = [], [], [[] for _ in range(nds)], [[] for _ in range(nds)]
    scerr, sgerr = [], []
    for r in runs:
        for kk in range(nds):
            key = "%s.%d" % (kind, kk)
            for m in r["scores"].get(key, []):
                fr.append(m["frac_maxerr"])
                sm.append(m["s_mean_rel_err"])
                tot[kk].append(m["total_rel_err"])
                rm[kk].append(m["rmsd"] / r["true_sigmas"][kk])
        if r["scales"]:
            scerr.append(max(abs(r["scales"][str(k)] / r["true_scales"][k] - 1.0)
                             for k in range(nds)))
        if r["sigmas"]:
            sgerr.append(max(abs(r["sigmas"][str(k)] / r["true_sigmas"][k] - 1.0)
                             for k in range(nds)))
    alltot = [t for tt in tot for t in tt]
    return {
        "n": len(runs),
        "frac": float(np.mean(fr)), "frac_sd": float(np.std(fr)),
        "s": float(np.mean(sm)), "s_sd": float(np.std(sm)),
        "tot": [float(np.mean(t)) for t in tot],
        "totR": float(math.sqrt(np.mean(np.square(alltot)))),
        "scale": float(np.max(scerr)) if scerr else float("nan"),
        "sigma": float(np.max(sgerr)) if sgerr else float("nan"),
        "rmsd": [float(np.mean(x)) for x in rm],
    }


def table(g, scen_order, kind="SC"):
    lines = ["| scenario | variant | n | max \\|Δfraction\\| | s̄ rel. err | total rel. err per dataset | total RMSE | max scale err | max σ̂ err | RMSD/σ per dataset |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for sn in scen_order:
        for vv in VORDER:
            if (sn, vv) not in g:
                continue
            st = stats(g[(sn, vv)], kind)
            lines.append("| %s | %s | %d | %.4f ± %.4f | %+.4f | %s | %.4f | %s | %s | %s |" % (
                sn, vv, st["n"], st["frac"], st["frac_sd"], st["s"],
                " / ".join("%+.4f" % t for t in st["tot"]), st["totR"],
                "—" if math.isnan(st["scale"]) else "%.4f" % st["scale"],
                "—" if math.isnan(st["sigma"]) else "%.4f" % st["sigma"],
                " / ".join("%.3f" % x for x in st["rmsd"])))
    return "\n".join(lines)


def figure(g, scen_order, fn, kind="SC"):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    colors = {"none": "#7f7f7f", "W": "#1f77b4", "S": "#ff7f0e", "WS": "#2ca02c"}
    metrics = [("frac", "max |Δ fraction|", False), ("s", "s̄ relative error", True),
               ("totR", "total conc. RMSE (per dataset)", False)]
    fig, axes = plt.subplots(len(metrics), 1, figsize=(15, 10), sharex=True)
    x = np.arange(len(scen_order))
    wid = 0.2
    for ai, (key, label, signed) in enumerate(metrics):
        ax = axes[ai]
        for vi, vv in enumerate(VORDER):
            vals, errs = [], []
            for sn in scen_order:
                if (sn, vv) in g:
                    st = stats(g[(sn, vv)], kind)
                    vals.append(st[key])
                    errs.append(st.get(key + "_sd", 0.0))
                else:
                    vals.append(np.nan)
                    errs.append(0.0)
            ax.bar(x + (vi - 1.5) * wid, np.abs(vals) if not signed else vals, wid,
                   yerr=errs if key in ("frac", "s") else None, color=colors[vv],
                   label=vv, capsize=2)
        ax.set_ylabel(label)
        if not signed:
            ax.set_yscale("log")
        ax.grid(alpha=0.3, axis="y")
        if signed:
            ax.axhline(0, color="k", lw=0.6)
    axes[0].legend(ncol=4, fontsize=9)
    axes[-1].set_xticks(x)
    axes[-1].set_xticklabels(scen_order, rotation=30, ha="right", fontsize=8)
    fig.suptitle("Global fit options vs. truth, %s models (real us_mpi_analysis; mean ± SD "
                 "over seeds)\nnone = defaults, W = gfit_weight, S = gfit_scale, WS = both" % kind)
    fig.tight_layout()
    fig.savefig(fn, dpi=110)


def diag_stats(runs):
    nds = len(runs[0]["true_scales"])
    dtot, dfr = [[] for _ in range(nds)], [[] for _ in range(nds)]
    gr = []
    for r in runs:
        sc_rm = []
        for kk in range(nds):
            for msc, mvr in zip(r["scores"].get("SC.%d" % kk, []),
                                r["scores"].get("VR.%d" % kk, [])):
                tsc, tvr = 1.0 + msc["total_rel_err"], 1.0 + mvr["total_rel_err"]
                dtot[kk].append(tsc / tvr - 1.0)
                dfr[kk].append(max(abs(a - b) for a, b in zip(msc["frac_err"], mvr["frac_err"])))
            m = r["scores"].get("SC.%d" % kk, [])
            if m:
                sc_rm.append(np.mean([x["rmsd"] ** 2 for x in m]))
        sig2 = np.mean(np.square(r["true_sigmas"]))
        gr.append(math.sqrt(np.mean(sc_rm) / sig2))
    return {"n": len(runs), "dtot": [float(np.mean(x)) for x in dtot],
            "dfrac": [float(np.mean(x)) for x in dfr], "grmsd": float(np.mean(gr))}


def diag_table(g, scen_order):
    lines = ["| scenario | variant | n | (SC−VR)/VR total per dataset | max \\|Δfraction SC−VR\\| per dataset | global RMSD/σ |",
             "|---|---|---|---|---|---|"]
    for sn in scen_order:
        for vv in VORDER:
            if (sn, vv) not in g:
                continue
            st = diag_stats(g[(sn, vv)])
            lines.append("| %s | %s | %d | %s | %s | %.3f |" % (
                sn, vv, st["n"], " / ".join("%+.4f" % x for x in st["dtot"]),
                " / ".join("%.4f" % x for x in st["dfrac"]), st["grmsd"]))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results", nargs="+")
    ap.add_argument("--fig")
    ap.add_argument("--md")
    args = ap.parse_args()
    rows = []
    for fn in args.results:
        rows += json.load(open(fn))
    g = collect(rows)
    scen_order = []
    for r in rows:
        if r["scenario"] not in scen_order:
            scen_order.append(r["scenario"])
    md = "\n\n".join([
        "### SC models (global solutes scaled to each dataset)", table(g, scen_order, "SC"),
        "### VR models (global solutes refit to each dataset)", table(g, scen_order, "VR"),
        "### Diagnostic: SC vs. VR and global RMSD", diag_table(g, scen_order)])
    print(md)
    if args.md:
        open(args.md, "w").write(md + "\n")
    if args.fig:
        figure(g, scen_order, args.fig, "SC")
        base, ext = os.path.splitext(args.fig)
        figure(g, scen_order, base + "_vr" + ext, "VR")


if __name__ == "__main__":
    main()
