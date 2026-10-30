#!/usr/bin/env python3
"""Plots for the global-fit study:  models of the current and the fixed
us_mpi_analysis side by side with the known truth, and the fits (data,
simulation of the output model, residuals) of each dataset.

Runs each scenario once (one seed) with both binaries, simulates every output
model on its dataset with gf_basis sim (US_Astfem_RSA), and writes
  models_<scenario>.png   pseudo-3D s vs f/f0, signal as marker area
  fits_<scenario>.png     data, model and residuals per dataset

Usage:  make_plots.py --testdata BIN --basis BIN --cur BIN --fix BIN
                      [--scenarios S0_baseline,...] [--seed 1] [--outdir plots]
"""
import argparse
import json
import os
import re
import subprocess
import tarfile
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

import run_e2e as E  # noqa: E402
import us_mpi_regression as R  # noqa: E402

DEFAULT = "S0_baseline,S1_odlimit_stray,S1b_odlimit_linear,S1c_low_odlimit,S2_ga_norm_cut"
WHAT = {
    "S0_baseline": "F3 only (no point above the OD limit)",
    "S1_odlimit_stray": "F1: stray light, OD limit 1.5, dataset 0 at 2.09 OD",
    "S1b_odlimit_linear": "F1: linear detector, OD limit 1.5, dataset 0 at 2.09 OD",
    "S1c_low_odlimit": "F1: OD limit 0.8 below the plateaus",
    "S2_ga_norm_cut": "F2: GA, 150 S aggregate pelleted in dataset 0 (60 krpm)",
}
COLORS = {"cur": "#c0392b", "fix": "#1f77b4"}


def run_case(scen_name, prog_name, program, args):
    scen = E.SCENARIOS[scen_name]
    tag = "%s_%s_plot" % (scen_name, prog_name)
    E.run_one(tag, scen, program, args.testdata, args.seed, args.workdir)
    casedir = Path(args.workdir) / tag
    mdir = casedir / "models"
    mdir.mkdir(exist_ok=True)
    with tarfile.open(casedir / "output" / "analysis-results.tar") as tar:
        for m in tar.getmembers():
            if m.name.endswith(".model.xml"):
                (mdir / os.path.basename(m.name)).write_bytes(
                    tar.extractfile(m).read())
    out = {"models": {}, "fits": {}}
    for kk, dd in enumerate(scen["datasets"]):
        for kind in ("SC", "VR"):
            files = sorted(mdir.glob("%s.*-GL-%s.*.model.xml" % (dd["run"], kind)))
            if not files:
                continue
            mf = files[0]
            root = R.ET.parse(mf).getroot()
            out["models"]["%s.%d" % (kind, kk)] = R.summarize_model(root)[0]
            prefix = casedir / ("sim_%s_%d" % (kind, kk))
            subprocess.run([args.basis, "sim", str(casedir / "inputs"), dd["run"],
                            str(mf), str(prefix)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            meta = json.loads(Path(str(prefix) + ".json").read_text())
            shape = (meta["nscans"], meta["npoints"])
            out["fits"]["%s.%d" % (kind, kk)] = {
                "r": np.array(meta["radius"]),
                "data": np.fromfile(str(prefix) + ".data.bin").reshape(shape),
                "sim": np.fromfile(str(prefix) + ".sim.bin").reshape(shape),
                "odlim": meta["ODlimit"],
            }
    return out


def truth_points(dd):
    agg = dd["opts"].get("agg", 0.0)
    total = 0.95 * dd["scale"]
    pts = [(s, k, f * (1.0 - agg) * total) for (s, k, f) in E.SPECIES]
    if agg > 0:
        pts.append((E.AGG[0], E.AGG[1], agg * total))
    return pts


def plot_models(scen_name, res, outdir):
    scen = E.SCENARIOS[scen_name]
    kinds = [k for k in ("SC.0", "VR.0", "SC.1", "VR.1") if k in res["cur"]["models"]]
    logx = any(dd["opts"].get("agg", 0) > 0 for dd in scen["datasets"])
    fig, axes = plt.subplots(len(kinds), 2, figsize=(11, 3.1 * len(kinds)),
                             sharex=True, sharey=True, squeeze=False)
    for row, kind in enumerate(kinds):
        kk = int(kind[-1])
        dd = scen["datasets"][kk]
        truth = truth_points(dd)
        cmax = max(max((c[3] for c in res[p]["models"][kind]["solutes"]), default=1)
                   for p in ("cur", "fix"))
        cmax = max(cmax, max(t[2] for t in truth))
        for col, prog in enumerate(("cur", "fix")):
            ax = axes[row][col]
            model = res[prog]["models"][kind]
            sols = np.array(model["solutes"]) if model["solutes"] else np.zeros((0, 4))
            if len(sols):
                ax.scatter(sols[:, 0] * 1e13, sols[:, 1], s=900 * sols[:, 3] / cmax,
                           c=COLORS[prog], alpha=0.55, edgecolors="k", linewidths=0.4,
                           label="fitted solutes")
            ts = np.array(truth)
            ax.scatter(ts[:, 0], ts[:, 1], s=900 * ts[:, 2] / cmax, facecolors="none",
                       edgecolors="#2ca02c", linewidths=1.8, label="truth")
            ax.scatter(ts[:, 0], ts[:, 1], marker="+", s=60, c="#2ca02c")
            tot = sols[:, 3].sum() if len(sols) else 0.0
            ttot = ts[:, 2].sum()
            rm = model["rmsd"]
            ax.set_title("%s  %s   total %.4f (truth %.4f, %+.1f %%)\n"
                         "%d solutes   reported RMSD %.5f (true σ %.4f)"
                         % (prog, kind, tot, ttot, 100 * (tot / ttot - 1),
                            len(sols), rm, float(dd["opts"].get("sigma", 0.004))),
                         fontsize=8.5)
            if logx:
                ax.set_xscale("log")
            ax.grid(alpha=0.3)
            if col == 0:
                ax.set_ylabel("f/f0")
            if row == len(kinds) - 1:
                ax.set_xlabel("s20,w [S]")
            if row == 0 and col == 0:
                ax.legend(fontsize=7, loc="upper left", markerscale=0.5)
    fig.suptitle("%s — %s\nSC = global solutes scaled to dataset, VR = refit to "
                 "dataset; marker area ∝ signal (OD)" % (scen_name, WHAT[scen_name]),
                 fontsize=10)
    fig.tight_layout()
    fn = Path(outdir) / ("models_%s.png" % scen_name)
    fig.savefig(fn, dpi=110)
    plt.close(fig)
    return fn


def plot_fits(scen_name, res, outdir):
    scen = E.SCENARIOS[scen_name]
    nds = len(scen["datasets"])
    cols = [("cur", "SC"), ("fix", "SC"), ("cur", "VR"), ("fix", "VR")]
    fig, axes = plt.subplots(2 * nds, 4, figsize=(17, 3.0 * 2 * nds), squeeze=False,
                             gridspec_kw={"height_ratios": [2, 1] * nds})
    for kk in range(nds):
        sigma = float(scen["datasets"][kk]["opts"].get("sigma", 0.004))
        for cc, (prog, kind) in enumerate(cols):
            key = "%s.%d" % (kind, kk)
            ax = axes[2 * kk][cc]
            axr = axes[2 * kk + 1][cc]
            if key not in res[prog]["fits"]:
                ax.axis("off")
                axr.axis("off")
                continue
            f = res[prog]["fits"][key]
            r, d, s = f["r"], f["data"], f["sim"]
            # OD limit in data units (us_mpi_analysis scales the data, the
            #  edit limit is in data units)
            odl = f["odlim"]
            dd = scen["datasets"][kk]
            cgiven = 0.95 * dd["scale"] * dd["conc_factor"]
            above = d >= odl
            # Points the binary excludes:  current code compares the
            #  normalized data with the unscaled limit (F1 not applied)
            excl = (d / cgiven >= odl) if prog == "cur" else above
            for ss in range(0, d.shape[0], 3):
                ax.plot(r, d[ss], ".", ms=1.2, color="0.55")
                ax.plot(r[excl[ss]], d[ss][excl[ss]], ".", ms=1.4, color="#ff7f0e")
                ax.plot(r, s[ss], "-", lw=0.8, color=COLORS[prog])
            if odl < 50:
                ax.axhline(odl, ls="--", lw=0.8, color="#ff7f0e")
            resid = d - s
            inc = ~excl
            rm_inc = np.sqrt((resid[inc] ** 2).mean())
            rm_all = np.sqrt((resid ** 2).mean())
            rep = res[prog]["models"][key]["rmsd"]
            ax.set_title("dataset %d  %s %s   OD limit %.2f: %d pts above, %d excluded"
                         " (orange)" % (kk, prog, kind, odl, above.sum(), excl.sum()),
                         fontsize=8.5)
            ax.set_ylabel("OD")
            for ss in range(d.shape[0]):
                axr.plot(r, resid[ss], lw=0.4, alpha=0.5, color=COLORS[prog])
            axr.axhspan(-2 * sigma, 2 * sigma, color="#2ca02c", alpha=0.15)
            lim = max(6 * sigma, min(np.abs(resid).max(), 0.15))
            axr.set_ylim(-lim, lim)
            axr.set_title("RMSD fitted pts %.5f, all pts %.5f, reported %.5f (σ %.4f)"
                          % (rm_inc, rm_all, rep, sigma), fontsize=8)
            axr.set_xlabel("radius [cm]")
            axr.set_ylabel("data − model")
    fig.suptitle("%s — fits per dataset: data (grey; orange = excluded by that build), "
                 "simulation of the output model (lines; every 3rd scan), residuals of all "
                 "scans (green band ±2σ)" % scen_name, fontsize=10)
    fig.tight_layout()
    fn = Path(outdir) / ("fits_%s.png" % scen_name)
    fig.savefig(fn, dpi=100)
    plt.close(fig)
    return fn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--testdata", required=True)
    ap.add_argument("--basis", required=True)
    ap.add_argument("--cur", required=True)
    ap.add_argument("--fix", required=True)
    ap.add_argument("--scenarios", default=DEFAULT)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--workdir", default="gf_plot_work")
    ap.add_argument("--outdir", default="plots")
    args = ap.parse_args()
    for a in ("testdata", "basis", "cur", "fix", "workdir", "outdir"):
        setattr(args, a, os.path.abspath(getattr(args, a)))
    Path(args.outdir).mkdir(parents=True, exist_ok=True)
    for sn in args.scenarios.split(","):
        res = {p: run_case(sn, p, getattr(args, p), args) for p in ("cur", "fix")}
        print(plot_models(sn, res, args.outdir))
        print(plot_fits(sn, res, args.outdir), flush=True)


if __name__ == "__main__":
    main()
