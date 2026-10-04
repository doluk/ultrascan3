"""Collect task results into CSV tables.

runs.csv        one row per task x arm (all scalar metrics)
species.csv     one row per task x arm x true solute
conditions.csv  mean and SD over replicates per condition x arm
paired.csv      per condition:  mean and SD over replicates of the paired
                difference of each arm to main_old (same simulated data)
errors.csv      failed task x arm runs
"""

import csv
import glob
import json
import math
import os
import shutil

from .design import ARMS, FACTORS

KEY_METRICS = [
    "rmsd", "rmsd_ratio", "noise_rmsd", "ti_shape_rmsd", "ri_shape_rmsd",
    "noise_offset", "corrected_data_rmsd", "total_conc_relerr",
    "ghost_conc_fraction", "species_found", "s_relerr_max",
    "ff0_relerr_max", "D_relerr_max", "mw_relerr_max", "conc_relerr_max",
    "s_relerr_mean", "ff0_relerr_mean", "conc_relerr_mean",
    "meniscus_fit_err", "bottom_fit_err", "fit_mb_on_edge", "edit_updated",
    "n_solutes_fit", "wall_seconds",
]
CONTEXT = ["sigma_random_true", "ti_true_rms", "ri_true_rms", "c_true_rms",
           "signal_mean", "points", "scans", "meniscus_edit_err",
           "bottom_edit_err"]


def _num(v):
    if isinstance(v, bool):
        return float(v)
    if isinstance(v, (int, float)):
        return float(v)
    return float("nan")


def _stats(vals):
    vals = [v for v in vals if not math.isnan(v)]
    n = len(vals)
    if n == 0:
        return float("nan"), float("nan"), 0
    mean = sum(vals) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in vals) / (n - 1)) \
        if n > 1 else 0.0
    return mean, sd, n


def _write(path, header, rows):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        w.writerows(rows)


def load_runs(outdir):
    runs = []
    for path in sorted(glob.glob(os.path.join(outdir, "*", "*.json"))):
        if os.path.basename(os.path.dirname(path)) == "summary":
            continue
        with open(path) as fh:
            runs.append(json.load(fh))
    return runs


def aggregate(outdir, dest):
    os.makedirs(dest, exist_ok=True)
    meta = os.path.join(outdir, "design.json")
    if os.path.exists(meta):
        shutil.copy(meta, os.path.join(dest, "design.json"))
    runs = load_runs(outdir)
    ok = [r for r in runs if r.get("status") == "ok"]
    bad = [r for r in runs if r.get("status") != "ok"]

    base = ["task", "condition", "replicate"] + FACTORS + ["blocks", "arm",
                                                           "build",
                                                           "workflow"]
    metrics = KEY_METRICS + CONTEXT
    rows = []
    for r in ok:
        t = r["task"]
        row = [t["task"], t["condition"], t["replicate"]] \
            + [t[f] for f in FACTORS] \
            + [";".join(t["blocks"]), r["arm"], r["build"], r["workflow"]]
        row += [r["metrics"].get(m, "") for m in metrics]
        rows.append(row)
    _write(os.path.join(dest, "runs.csv"), base + metrics, rows)

    srows = []
    for r in ok:
        t = r["task"]
        for j, sp in enumerate(r["metrics"]["species"]):
            srows.append([t["task"], t["condition"], t["system"], t["speed"],
                          r["arm"], j + 1, sp["s_true"], sp["ff0_true"],
                          sp["conc_true"], sp["conc"], sp.get("s", ""),
                          sp.get("ff0", ""), sp.get("conc_relerr", ""),
                          sp.get("s_relerr", ""), sp.get("ff0_relerr", ""),
                          sp.get("D_relerr", ""), sp.get("mw_relerr", ""),
                          sp["n_solutes"]])
    _write(os.path.join(dest, "species.csv"),
           ["task", "condition", "system", "speed", "arm", "solute",
            "s_true", "ff0_true", "conc_true", "conc_fit", "s_fit",
            "ff0_fit", "conc_relerr", "s_relerr", "ff0_relerr", "D_relerr",
            "mw_relerr", "n_solutes"], srows)

    _write(os.path.join(dest, "errors.csv"), ["task", "arm", "error"],
           [[r["task"]["task"], r["arm"], r.get("error", "")] for r in bad])

    # Replicate statistics per condition x arm
    by = {}
    tasks = {}
    for r in ok:
        key = (r["task"]["condition"], r["arm"])
        by.setdefault(key, []).append(r)
        tasks[r["task"]["condition"]] = r["task"]
    head = ["condition"] + FACTORS + ["blocks", "arm", "n"]
    for m in metrics:
        head += [m + "_mean", m + "_sd"]
    crow = []
    for (cond, arm), lst in sorted(by.items()):
        t = tasks[cond]
        row = [cond] + [t[f] for f in FACTORS] + [";".join(t["blocks"]),
                                                 arm, len(lst)]
        for m in metrics:
            mean, sd, _ = _stats([_num(x["metrics"].get(m)) for x in lst])
            row += [mean, sd]
        crow.append(row)
    _write(os.path.join(dest, "conditions.csv"), head, crow)

    # Paired differences to main_old
    per_task = {}
    for r in ok:
        per_task.setdefault(r["task"]["task"], {})[r["arm"]] = r
    pd = {}
    for tid, arms in per_task.items():
        if "main_old" not in arms:
            continue
        ref = arms["main_old"]["metrics"]
        for arm, r in arms.items():
            if arm == "main_old":
                continue
            key = (r["task"]["condition"], arm)
            pd.setdefault(key, []).append(
                {m: _num(r["metrics"].get(m)) - _num(ref.get(m))
                 for m in KEY_METRICS})
    head = ["condition"] + FACTORS + ["blocks", "arm", "n"]
    for m in KEY_METRICS:
        head += ["d_" + m + "_mean", "d_" + m + "_sd"]
    prow = []
    for (cond, arm), lst in sorted(pd.items()):
        t = tasks[cond]
        row = [cond] + [t[f] for f in FACTORS] + [";".join(t["blocks"]),
                                                 arm, len(lst)]
        for m in KEY_METRICS:
            mean, sd, _ = _stats([d[m] for d in lst])
            row += [mean, sd]
        prow.append(row)
    _write(os.path.join(dest, "paired.csv"), head, prow)

    print("runs ok %d, failed %d -> %s" % (len(ok), len(bad), dest))
    present = sorted(set(r["arm"] for r in ok), key=ARMS.index)
    print("arms: " + ", ".join(present))
