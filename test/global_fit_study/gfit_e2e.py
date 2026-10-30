#!/usr/bin/env python3
"""Known-truth tests of the global-fit options gfit_weight (sigma weighting)
and gfit_scale (fitted signal scale per dataset) in us_mpi_analysis.

One binary, four variants on identical data (same seeds):
  none   defaults (unweighted, given total concentrations)
  W      gfit_weight=1
  S      gfit_scale=1
  WS     gfit_weight=1, gfit_scale=1

Every per-dataset model scaled from the global solutes (SC.k) is scored
against the dataset's own truth:  total concentration, signal fractions in s
windows, signal-weighted mean s; the fitted scales and the noise estimates
are read from the run log.

Usage:
  gfit_e2e.py --testdata BIN --program BIN [--reps N] [--scenarios A1,...]
              [--variants none,W,S,WS] [--jobs 2] [--out results.json]
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import tarfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import run_e2e as E
import us_mpi_regression as R

# Species of gf_testdata (20,W):  s [S], f/f0, fraction of the main signal
MAIN = [(2.5, 1.25, 0.25 / 0.95), (4.5, 1.6, 0.40 / 0.95), (7.0, 2.1, 0.30 / 0.95)]
EXTRA = (9.0, 1.4)
AGG = (150.0, 1.5)
WINDOWS = [(0.0, 3.5), (3.5, 5.75), (5.75, 8.0), (8.0, 50.0), (50.0, 1e4)]

VARIANTS = {
    "none": {},
    "W": {"gfit_weight": "1"},
    "S": {"gfit_scale": "1"},
    "WS": {"gfit_weight": "1", "gfit_scale": "1"},
}

ds = E.ds
G2 = dict(E.GRID_2DSA)

SCENARIOS = {
    # A: correct concentrations, nothing to correct (no-harm checks)
    "A1_identical": [ds("gfA", 1.0), ds("gfB", 1.0)],
    "A2_three_conc": [ds("gfA", 1.0), ds("gfB", 0.5), ds("gfC", 0.25)],
    # B: weighting
    "B1_dilute_equal_sigma": [ds("gfA", 1.0), ds("gfB", 0.1)],
    "B2_sigma_prop_conc": [ds("gfA", 1.0), ds("gfB", 0.2, sigma=0.0008)],
    "B3_diff_optics": [ds("gfA", 1.0), ds("gfB", 1.0, sigma=0.012)],
    # C: wrong given total concentrations
    "C1_conc_p10_same_speed": [ds("gfA", 1.0), ds("gfB", 0.5, conc_factor=1.10)],
    "C2_conc_m10_multispeed": [ds("gfA", 1.0), ds("gfB", 0.5, conc_factor=0.90, rpm=30000)],
    "C3_conc_p30_multispeed": [ds("gfA", 1.0), ds("gfB", 0.5, conc_factor=1.30, rpm=30000)],
    "C4_three_speeds_errors": [ds("gfA", 1.0, conc_factor=1.15),
                               ds("gfB", 0.6, rpm=40000),
                               ds("gfC", 0.4, conc_factor=0.90, rpm=30000)],
    "C5_common_error_p20": [ds("gfA", 1.0, conc_factor=1.2),
                            ds("gfB", 0.5, conc_factor=1.2, rpm=30000)],
    # D: robustness
    "D1_extra_species_in_B": [ds("gfA", 1.0), ds("gfB", 1.0, extra=0.10)],
    "D2_ti_noise_unfitted": [ds("gfA", 1.0, ti=0.004), ds("gfB", 0.3, ti=0.004)],
    "D3_very_dilute_conc_err": [ds("gfA", 1.0), ds("gfB", 0.03, conc_factor=0.9)],
    "D4_slow_low_info": [ds("gfA", 1.0), ds("gfB", 0.5, conc_factor=1.1, rpm=15000)],
    # G: composition differs between datasets (S/WS assume the same)
    "G1_extra_species_in_A": [ds("gfA", 1.0, extra=0.10), ds("gfB", 1.0)],
    "G2_three_extra_in_C": [ds("gfA", 1.0), ds("gfB", 1.0), ds("gfC", 1.0, extra=0.10)],
    "G3_three_extra_in_C_dilute": [ds("gfA", 1.0), ds("gfB", 1.0),
                                   ds("gfC", 0.3, extra=0.10)],
    "G4_three_extra_in_C_slow": [ds("gfA", 1.0), ds("gfB", 1.0),
                                 ds("gfC", 1.0, extra=0.10, rpm=30000)],
    "G5_three_extra_in_A_loaded": [ds("gfA", 1.0, extra=0.10), ds("gfB", 0.3),
                                   ds("gfC", 0.3, rpm=30000)],
}
EXTRA_SCEN = {
    # Monte Carlo with both options
    "E1_mc_conc_m10_multispeed": {
        "method": "2DSA", "np": 2, "params": dict(G2, mc_iterations="5"),
        "datasets": SCENARIOS["C2_conc_m10_multispeed"]},
    # GA global fit with both options
    "F1_ga_conc_m10_multispeed": {
        "method": "GA", "np": 4, "params": dict(E.GA_PARAMS), "buckets": list(R.GA_BUCKETS),
        "datasets": SCENARIOS["C2_conc_m10_multispeed"]},
}


def scenario(name):
    if name in EXTRA_SCEN:
        return EXTRA_SCEN[name]
    return {"method": "2DSA", "np": 2, "params": G2, "datasets": SCENARIOS[name]}


def truth_of(dd):
    agg = dd["opts"].get("agg", 0.0)
    extra = dd["opts"].get("extra", 0.0)
    total = 0.95 * dd["scale"]
    comps = [(s, k, f * (1.0 - agg - extra)) for (s, k, f) in MAIN]
    if extra > 0:
        comps.append((EXTRA[0], EXTRA[1], extra))
    if agg > 0:
        comps.append((AGG[0], AGG[1], agg))
    fr = [sum(f for (s, _, f) in comps if lo <= s < hi) for lo, hi in WINDOWS]
    smean = sum(s * f for (s, _, f) in comps)
    return {"total": total, "fractions": fr, "s_mean": smean}


def run_case(tag, scen, variant, program, testdata, seed, workdir):
    casedir = Path(workdir) / tag
    if casedir.exists():
        shutil.rmtree(casedir)
    inputs = casedir / "inputs"
    inputs.mkdir(parents=True)
    for kk, dd in enumerate(scen["datasets"]):
        opts = ["%s=%s" % (k, v) for k, v in dd["opts"].items()]
        subprocess.run([testdata, "data", str(inputs), dd["run"], "1",
                        str(seed * 10 + kk), str(dd["scale"])] + opts,
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    scen_v = dict(scen, params=dict(scen["params"], **VARIANTS[variant]))
    (inputs / "hpcrequest-local-gfstudy-1.xml").write_text(E.job_xml(scen_v, seed))
    archive = casedir / ("%s.tar" % tag)
    with tarfile.open(archive, "w") as tar:
        for path in sorted(inputs.iterdir()):
            tar.add(path, arcname=path.name)
    env = dict(os.environ)
    env.setdefault("OMPI_ALLOW_RUN_AS_ROOT", "1")
    env.setdefault("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM", "1")
    env.setdefault("OMPI_MCA_rmaps_base_oversubscribe", "1")
    env["QT_LOGGING_RULES"] = "*.debug=true"
    cmd = R.mpirun_command("mpirun", scen["np"]) + [
        program, "-walltime", "60", "-mgroupcount", "1", archive.name]
    log = casedir / "run.log"
    with open(log, "w") as out:
        proc = subprocess.run(cmd, cwd=casedir, env=env, stdout=out,
                              stderr=subprocess.STDOUT, timeout=3600)
    results = casedir / "output" / "analysis-results.tar"
    if proc.returncode != 0 or not results.exists():
        raise RuntimeError("%s failed, see %s" % (tag, log))
    text = log.read_text(errors="replace")
    # Last reported scales (final write) and the noise estimates
    scales = {}
    for m in re.finditer(r"Global fit dataset (\d+) signal scale ([0-9.eE+-]+)", text):
        scales[int(m.group(1)) - 1] = float(m.group(2))
    sigmas = {}
    for m in re.finditer(r"Global fit dataset (\d+) total_concentration \S+ noise sigma ([0-9.eE+-]+)", text):
        sigmas[int(m.group(1)) - 1] = float(m.group(2))
    return R.summarize(results), scales, sigmas


def score(summary, scen):
    out = {}
    runs = [dd["run"] for dd in scen["datasets"]]
    for key, models in summary["models"].items():
        kind = "SG" if "-SG" in key else ("SC" if "-SC" in key else "VR")
        if kind == "SG":
            continue
        kk = next(ii for ii, run in enumerate(runs) if key.startswith(run + "."))
        dd = scen["datasets"][kk]
        truth = truth_of(dd)
        res = []
        for model in models:          # Several for Monte Carlo
            sols = model["solutes"]
            tot = sum(c[3] for c in sols)
            fr = [sum(c[3] for c in sols if lo <= c[0] * 1e13 < hi) / tot for lo, hi in WINDOWS]
            smean = sum(c[0] * 1e13 * c[3] for c in sols) / tot
            res.append({
                "rmsd": model["rmsd"],
                "total_rel_err": tot / truth["total"] - 1.0,
                "frac_err": [a - b for a, b in zip(fr, truth["fractions"])],
                "frac_maxerr": max(abs(a - b) for a, b in zip(fr, truth["fractions"])),
                "s_mean_rel_err": smean / truth["s_mean"] - 1.0,
            })
        out["%s.%d" % (kind, kk)] = res
    return out


def true_scales(scen):
    # Fitted scale should be (c_true/c_given) normalized to geometric mean 1
    r = [1.0 / dd["conc_factor"] for dd in scen["datasets"]]
    g = math.exp(sum(math.log(x) for x in r) / len(r))
    return [x / g for x in r]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--testdata", required=True)
    ap.add_argument("--program", required=True)
    ap.add_argument("--reps", type=int, default=8)
    ap.add_argument("--scenarios", default="all")
    ap.add_argument("--variants", default="none,W,S,WS")
    ap.add_argument("--workdir", default="gfit_work")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--out", default="gfit_results.json")
    args = ap.parse_args()
    program = os.path.abspath(args.program)
    testdata = os.path.abspath(args.testdata)
    names = (list(SCENARIOS) + list(EXTRA_SCEN)) if args.scenarios == "all" \
        else args.scenarios.split(",")
    variants = args.variants.split(",")
    tasks = [(sn, vv, seed) for sn in names for vv in variants
             for seed in range(1, args.reps + 1)]

    def work(task):
        sn, vv, seed = task
        scen = scenario(sn)
        tag = "%s_%s_%d" % (sn, vv, seed)
        summ, scales, sigmas = run_case(tag, scen, vv, program, testdata, seed, args.workdir)
        shutil.rmtree(Path(args.workdir) / tag, ignore_errors=True)
        return {"scenario": sn, "variant": vv, "seed": seed,
                "scores": score(summ, scen), "scales": scales, "sigmas": sigmas,
                "true_scales": true_scales(scen),
                "true_sigmas": [float(dd["opts"].get("sigma", 0.004)) for dd in scen["datasets"]]}

    results = []
    outp = Path(args.out)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for res in pool.map(work, tasks):
            results.append(res)
            print("done", res["scenario"], res["variant"], res["seed"], flush=True)
            outp.write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
