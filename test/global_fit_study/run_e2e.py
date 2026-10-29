#!/usr/bin/env python3
"""End-to-end global-fit study: run the real us_mpi_analysis on synthetic
datasets with known truth and score the output models against the truth.

Each scenario is run with one or more us_mpi_analysis binaries (e.g. the
current code and a build with the fixes in gf_fixes.patch) over several noise
seeds.  Data are written by gf_testdata (same simulation path as the
regression data of test/mpi_analysis), the job archive and the result parsing
reuse test/mpi_analysis/us_mpi_regression.py.

Usage:
  run_e2e.py --testdata BIN --program NAME=BIN [--program NAME=BIN ...]
             [--reps N] [--scenarios S1,S2] [--workdir DIR] [--out results.json]
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "mpi_analysis"))
import us_mpi_regression as R  # noqa: E402

# Truth of gf_testdata (20,W):  s [S], f/f0, fraction of the main signal
SPECIES = [(2.5, 1.25, 0.25 / 0.95), (4.5, 1.6, 0.40 / 0.95), (7.0, 2.1, 0.30 / 0.95)]
AGG = (150.0, 1.5)
# s windows [S] used to integrate the fitted signal of each species
WINDOWS = [(0.0, 3.5), (3.5, 5.75), (5.75, 50.0), (50.0, 1.0e4)]

GRID_2DSA = dict(R.GRID_2DSA)

GA_PARAMS = {
    "population": "40", "generations": "30", "crossover": "50",
    "mutation": "50", "plague": "4", "elitism": "2",
    "migration": "3", "regularization": "5",
    "conc_threshold": "0.000001", "s_grid": "100", "k_grid": "100",
    "mutate_sigma": "2", "p_mutate_s": "20", "p_mutate_k": "20",
    "p_mutate_sk": "20", "minimize_opt": "0",
    "mc_iterations": "1", "tinoise_option": "0", "rinoise_option": "0",
    "debug_level": "0",
}
GA_BUCKETS = list(R.GA_BUCKETS) + [(100.0, 200.0, 1.2, 1.8)]


def ds(run, scale, conc_factor=1.0, **opts):
    """A dataset:  loading scale (total OD = 0.95 * scale), the factor applied
    to the true total concentration given to the job, gf_testdata options."""
    return {"run": run, "scale": scale, "conc_factor": conc_factor,
            "opts": opts}


# Scenario definitions.  "truth" per dataset is derived from scale and agg.
SCENARIOS = {
    # Linear detector, no point near the OD limit:  reference for F3
    "S0_baseline": {
        "method": "2DSA", "np": 2, "params": GRID_2DSA,
        "datasets": [ds("gfA", 1.0), ds("gfB", 0.3)],
    },
    # Concentrated dataset with detector stray light (0.5 %) above the
    #  OD limit 1.5:  F1 decides whether the nonlinear points are excluded
    "S1_odlimit_stray": {
        "method": "2DSA", "np": 2, "params": GRID_2DSA,
        "datasets": [ds("gfA", 2.2, stray=0.005), ds("gfB", 0.6, stray=0.005)],
    },
    # The same with a linear detector:  points above the OD limit are valid,
    #  so excluding them (F1) only loses information
    "S1b_odlimit_linear": {
        "method": "2DSA", "np": 2, "params": GRID_2DSA,
        "datasets": [ds("gfA", 2.2), ds("gfB", 0.6)],
    },
    # OD limit below the plateau of the normalized data (0.8):  the current
    #  code excludes the plateau of every dataset, also of the dilute one
    "S1c_low_odlimit": {
        "method": "2DSA", "np": 2, "params": GRID_2DSA,
        "datasets": [ds("gfA", 1.0, stray=0.005, odlim=0.8),
                     ds("gfB", 0.4, stray=0.005, odlim=0.8)],
    },
    # Total concentration given 10 % too high for dataset B
    "S3_conc_error": {
        "method": "2DSA", "np": 2, "params": GRID_2DSA,
        "datasets": [ds("gfA", 1.0), ds("gfB", 0.5, conc_factor=1.10)],
    },
    # GA global fit, 60 krpm dataset first:  the 150 S aggregate is pelleted
    #  before the first scan there (A column cut by dset_matrices), visible
    #  in the 20 krpm dataset (F2)
    "S2_ga_norm_cut": {
        "method": "GA", "np": 4, "params": GA_PARAMS, "buckets": GA_BUCKETS,
        "datasets": [ds("gfA", 1.0, agg=0.1),
                     ds("gfB", 1.0, agg=0.1, rpm=20000)],
    },
}


def job_xml(scen, seed):
    root = ET.Element("US_JobSubmit", method=scen["method"], version="1.0")
    job = R.sub(root, "job")
    R.sub(job, "cluster", name="local", shortname="local")
    R.sub(job, "name", value="us3_gfstudy")
    R.sub(job, "udp", server="127.0.0.1", port="12233")
    R.sub(job, "global_fit", value="1")
    R.sub(job, "request", id="1", guid="00000000-0000-4000-8000-000000000001")
    params = R.sub(job, "jobParameters")
    for key, value in dict(scen["params"], seed=str(seed)).items():
        R.sub(params, key, value=value)
    for (xmin, xmax, ymin, ymax) in scen.get("buckets", []):
        R.sub(params, "bucket", s_min=xmin, s_max=xmax, ff0_min=ymin,
              ff0_max=ymax)
    for dd in scen["datasets"]:
        triple = "RI.1.A.260"
        dset = R.sub(root, "dataset")
        files = R.sub(dset, "files")
        R.sub(files, "auc", filename="%s.%s.auc" % (dd["run"], triple))
        R.sub(files, "edit", filename="%s.e1.%s.xml" % (dd["run"], triple))
        sol = R.sub(dset, "solution")
        R.sub(sol, "buffer", density="0.998234", viscosity="1.00194",
              compress="0", manual="0")
        R.sub(sol, "analyte", mw="50000", vbar20="0.72", amount="1",
              type="Protein")
        R.sub(dset, "band_volume", value="0.0")
        R.sub(dset, "rotor_stretch", value="0 1e-12")
        R.sub(dset, "centerpiece_bottom", value="7.2")
        R.sub(dset, "centerpiece_shape", value="standard")
        R.sub(dset, "centerpiece_angle", value="2.5")
        R.sub(dset, "centerpiece_pathlength", value="1.2")
        R.sub(dset, "centerpiece_width", value="0")
        R.sub(dset, "total_concentration",
              value="%.6f" % (0.95 * dd["scale"] * dd["conc_factor"]))
    return ET.tostring(root, encoding="unicode")


def run_one(name, scen, program, testdata, seed, workdir):
    casedir = Path(workdir) / name
    if casedir.exists():
        shutil.rmtree(casedir)
    inputs = casedir / "inputs"
    inputs.mkdir(parents=True)
    for kk, dd in enumerate(scen["datasets"]):
        opts = ["%s=%s" % (k, v) for k, v in dd["opts"].items()]
        subprocess.run([testdata, "data", str(inputs), dd["run"], "1",
                        str(seed * 10 + kk), str(dd["scale"])] + opts,
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    (inputs / "hpcrequest-local-gfstudy-1.xml").write_text(job_xml(scen, seed))
    archive = casedir / ("%s.tar" % name)
    with tarfile.open(archive, "w") as tar:
        for path in sorted(inputs.iterdir()):
            tar.add(path, arcname=path.name)

    env = dict(os.environ)
    env.setdefault("OMPI_ALLOW_RUN_AS_ROOT", "1")
    env.setdefault("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM", "1")
    env.setdefault("OMPI_MCA_rmaps_base_oversubscribe", "1")
    env["QT_LOGGING_RULES"] = "*.debug=false"
    cmd = R.mpirun_command("mpirun", scen["np"]) + [
        program, "-walltime", "60", "-mgroupcount", "1", archive.name]
    with open(casedir / "run.log", "w") as out:
        proc = subprocess.run(cmd, cwd=casedir, env=env, stdout=out,
                              stderr=subprocess.STDOUT, timeout=1200)
    results = casedir / "output" / "analysis-results.tar"
    if proc.returncode != 0 or not results.exists():
        raise RuntimeError("%s failed, see %s" % (name, casedir / "run.log"))
    return R.summarize(results)


def truth_of(dd):
    agg = dd["opts"].get("agg", 0.0)
    total = 0.95 * dd["scale"]
    fr = [f * (1.0 - agg) for (_, _, f) in SPECIES] + [agg]
    smean = sum(s * f for (s, _, _), f in zip(SPECIES, fr)) + AGG[0] * agg
    return {"total": total, "fractions": fr, "s_mean": smean}


def score_model(model, truth, sigma_true):
    sols = model["solutes"]          # [s, f/f0, vbar, signal], s in sec
    total = sum(c[3] for c in sols)
    fr = []
    for lo, hi in WINDOWS:
        sig = sum(c[3] for c in sols if lo <= c[0] * 1e13 < hi)
        fr.append(sig / total if total > 0 else float("nan"))
    smean = (sum(c[0] * 1e13 * c[3] for c in sols) / total) if total else 0
    return {
        "rmsd_reported": model["rmsd"],
        "rmsd_true_noise": sigma_true,
        "total": total, "total_rel_err": total / truth["total"] - 1.0,
        "fractions": fr,
        "frac_abs_err": max(abs(a - b) for a, b in zip(fr, truth["fractions"])),
        "s_mean": smean, "s_mean_rel_err": smean / truth["s_mean"] - 1.0,
        "agg_signal": sum(c[3] for c in sols if c[0] * 1e13 >= 50.0),
        "nsolutes": len(sols),
    }


def score(summary, scen):
    """Score per output model kind:  SG (superglobal), SC.k (global solutes
    scaled to dataset k), VR.k (refit to dataset k)."""
    out = {}
    runs = [dd["run"] for dd in scen["datasets"]]
    for key, models in summary["models"].items():
        model = models[0]
        kind = "SG" if "-SG." in key else ("SC" if "-SC." in key else "VR")
        kk = 0
        for ii, run in enumerate(runs):
            if key.startswith(run + "."):
                kk = ii
        dd = scen["datasets"][kk]
        truth = truth_of(dd)
        if kind == "SG":
            # Superglobal: concentrations scaled by the mean concentration
            mean_tot = sum(truth_of(d)["total"] for d in scen["datasets"]) \
                / len(scen["datasets"])
            truth = dict(truth, total=mean_tot)
            label = "SG"
        else:
            label = "%s.%d" % (kind, kk)
        sigma = float(dd["opts"].get("sigma", 0.004))
        out[label] = score_model(model, truth, sigma)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--testdata", required=True)
    ap.add_argument("--program", action="append", required=True,
                    help="NAME=path of a us_mpi_analysis binary")
    ap.add_argument("--reps", type=int, default=8)
    ap.add_argument("--scenarios", default="all")
    ap.add_argument("--workdir", default="gf_work")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--out", default="e2e_results.json")
    args = ap.parse_args()

    programs = {n: os.path.abspath(p) for n, p in
                (q.split("=", 1) for q in args.program)}
    args.testdata = os.path.abspath(args.testdata)
    names = list(SCENARIOS) if args.scenarios == "all" \
        else args.scenarios.split(",")
    tasks = [(sn, pn, seed) for sn in names for pn in programs
             for seed in range(1, args.reps + 1)]

    def work(task):
        sn, pn, seed = task
        scen = SCENARIOS[sn]
        tag = "%s_%s_%d" % (sn, pn, seed)
        summ = run_one(tag, scen, programs[pn], args.testdata, seed,
                       args.workdir)
        shutil.rmtree(Path(args.workdir) / tag, ignore_errors=True)
        return {"scenario": sn, "program": pn, "seed": seed,
                "scores": score(summ, scen)}

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for res in pool.map(work, tasks):
            results.append(res)
            print("done", res["scenario"], res["program"], res["seed"],
                  flush=True)
    Path(args.out).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
