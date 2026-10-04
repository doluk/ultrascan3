"""Run one task:  simulate it, run the selected arms, evaluate, store."""

import json
import os
import shutil
import time
import traceback

from . import design as dsg
from . import evaluate, sim, workflow


def load_config(path):
    with open(path) as fh:
        cfg = json.load(fh)
    for key in ("astfem_sim", "mpi_analysis_main", "mpi_analysis_branch",
                "mpirun", "scratch", "us3_settings"):
        if key not in cfg:
            raise KeyError("config is missing '%s'" % key)
    # Programs run in per-task directories:  make every path absolute
    # (relative to the config file's directory)
    base = os.path.dirname(os.path.abspath(path))
    for key in ("astfem_sim", "mpi_analysis_main", "mpi_analysis_branch",
                "scratch", "us3_settings"):
        cfg[key] = os.path.normpath(os.path.join(
            base, os.path.expanduser(cfg[key])))
    cfg.setdefault("sim_setup", "")
    cfg.setdefault("mpi_setup", "")
    cfg.setdefault("sim_timeout", 900)
    return cfg


def us3_env(cfg, root):
    """Environment with a private UltraScan settings store and work tree.

    us_astfem_sim needs a registered license (cfg['us3_settings'] is a
    copy of the user's UltraScan settings file) and an existing tmp
    directory for its time state; a private store keeps parallel tasks
    from sharing (and rewriting) one settings file.
    """
    sdir = os.path.join(root, "settings", "UltraScan3")
    work = os.path.join(root, "ultrascan")
    os.makedirs(sdir, exist_ok=True)
    for sub in ("tmp", "data", "results", "reports", "etc", "archive"):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    lines = []
    with open(cfg["us3_settings"]) as fh:
        for line in fh:
            if not line.startswith(("workBaseDir=", "tmpDir=")):
                lines.append(line.rstrip("\n"))
    if "[General]" not in lines:
        lines.insert(0, "[General]")
    pos = lines.index("[General]") + 1
    lines[pos:pos] = ["workBaseDir=" + work, "tmpDir=" + work + "/tmp"]
    with open(os.path.join(sdir, "UltraScan.ini"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    env = dict(os.environ)
    env["US3_SETTINGS_ROOT"] = os.path.join(root, "settings")
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["HOME"] = root
    return env


ARM_SPEC = {
    "main_old": ("mpi_analysis_main", "old"),
    "main_new": ("mpi_analysis_main", "new"),
    "branch_old": ("mpi_analysis_branch", "old"),
    "branch_new": ("mpi_analysis_branch", "new"),
}


def result_path(outdir, task, arm):
    return os.path.join(outdir, task["task"], arm + ".json")


def run_task(design, task, cfg, outdir, arms=None, keep=False,
             timeout=None):
    outdir = os.path.abspath(outdir)
    arms = arms or dsg.ARMS
    todo = [a for a in arms if not os.path.exists(result_path(outdir, task,
                                                              a))]
    if not todo:
        return []
    root = os.path.join(cfg["scratch"], task["task"])
    if os.path.exists(root):
        shutil.rmtree(root)
    os.makedirs(root)
    log = os.path.join(root, "log.txt")
    os.makedirs(os.path.join(outdir, task["task"]), exist_ok=True)
    meta = os.path.join(outdir, "design.json")
    if not os.path.exists(meta):
        with open(meta, "w") as fh:
            json.dump(design, fh, indent=1)
    done = []
    try:
        env = us3_env(cfg, root)
        t0 = time.time()
        truth = sim.simulate(task, design, cfg["astfem_sim"],
                             os.path.join(root, "sim"), env, log,
                             cfg["sim_setup"], cfg["sim_timeout"])
        sim_seconds = time.time() - t0
        for arm in todo:
            exe_key, wf = ARM_SPEC[arm]
            rec = {"task": task, "arm": arm, "build": exe_key[13:],
                   "workflow": wf, "sim_seconds": sim_seconds,
                   "truth": {k: v for k, v in truth.items()
                             if k not in ("ti_noise", "ri_noise")}}
            try:
                res = workflow.run_arm(
                    task, design, truth, truth["data_dir"], cfg[exe_key],
                    cfg["mpirun"], os.path.join(root, arm), log, wf,
                    timeout, cfg["mpi_setup"])
                rec["metrics"] = evaluate.evaluate(truth, res)
                rec["fit_mb"] = res["fit_mb"]
                rec["fit_mb_grid"] = res["fit_mb_grid"]
                rec["steps"] = res["steps"]
                rec["final_components"] = res["final"]["components"]
                rec["status"] = "ok"
            except Exception as exc:          # keep going with other arms
                rec["status"] = "error"
                rec["error"] = "%s: %s" % (type(exc).__name__, exc)
                rec["traceback"] = traceback.format_exc()
            with open(result_path(outdir, task, arm), "w") as fh:
                json.dump(rec, fh, indent=1, default=float)
            done.append((arm, rec["status"]))
    finally:
        if os.path.exists(log):
            shutil.copy(log, os.path.join(outdir, task["task"], "log.txt"))
        if not keep:
            shutil.rmtree(root, ignore_errors=True)
    return done
