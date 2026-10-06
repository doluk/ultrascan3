"""Run one task:  simulate it, run the selected arms, evaluate, store."""

import json
import os
import shutil
import socket
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


# Design settings that change the result of a task with a given task id
RESULT_KEYS = ("base_seed", "simulation", "edit", "analysis")


def check_outdir(design, outdir):
    """Record the design in outdir; refuse an outdir holding results of a
    design with other simulation/analysis settings (same task ids would
    otherwise be skipped as done)."""
    os.makedirs(outdir, exist_ok=True)
    meta = os.path.join(outdir, "design.json")
    if os.path.exists(meta):
        with open(meta) as fh:
            old = json.load(fh)
        diff = [k for k in RESULT_KEYS if old.get(k) != design.get(k)]
        if diff:
            raise SystemExit(
                "%s holds results of design '%s' with different %s; use "
                "another output directory" % (outdir, old.get("name"),
                                              ", ".join(diff)))
    else:
        with open(meta, "w") as fh:
            json.dump(design, fh, indent=1)


def _write_json(path, obj):
    """Write atomically, so a concurrent status never sees partial files."""
    tmp = "%s.tmp%d" % (path, os.getpid())
    with open(tmp, "w") as fh:
        json.dump(obj, fh, indent=1, default=float)
    os.replace(tmp, path)


def _remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def simulation_failed(outdir, task, arm):
    """True if the result of this arm failed in the simulation step."""
    try:
        with open(result_path(outdir, task, arm)) as fh:
            return json.load(fh).get("error", "").startswith("simulation:")
    except (OSError, ValueError):
        return False


def reset_failed(outdir, only_simulation=False):
    """Delete failed results and the claims of their tasks."""
    import glob
    removed, tasks = 0, set()
    for f in glob.glob(os.path.join(outdir, "*", "*.json")):
        if os.path.basename(f)[:-5] not in dsg.ARMS:
            continue
        try:
            with open(f) as fh:
                rec = json.load(fh)
        except (OSError, ValueError):
            continue
        if rec.get("status") == "ok":
            continue
        if only_simulation and \
                not rec.get("error", "").startswith("simulation:"):
            continue
        os.remove(f)
        removed += 1
        tasks.add(os.path.basename(os.path.dirname(f)))
    for t in tasks:
        shutil.rmtree(os.path.join(outdir, ".claims", t), ignore_errors=True)
    print("removed %d failed results of %d tasks (claims released)"
          % (removed, len(tasks)))
    return 0


def result_path(outdir, task, arm):
    return os.path.join(outdir, task["task"], arm + ".json")


def _done(path, retry_failed):
    if not os.path.exists(path):
        return False
    if not retry_failed:
        return True
    try:
        with open(path) as fh:
            return json.load(fh).get("status") == "ok"
    except (OSError, ValueError):
        return False


def run_task(design, task, cfg, outdir, arms=None, keep=False,
             timeout=None, retry_failed=False):
    outdir = os.path.abspath(outdir)
    arms = arms or dsg.ARMS
    todo = [a for a in arms
            if not _done(result_path(outdir, task, a), retry_failed)]
    if not todo:
        return []
    root = os.path.join(cfg["scratch"], task["task"])
    if os.path.exists(root):
        shutil.rmtree(root)
    os.makedirs(root)
    log = os.path.join(root, "log.txt")
    os.makedirs(os.path.join(outdir, task["task"]), exist_ok=True)
    check_outdir(design, outdir)
    done = []
    try:
        env = us3_env(cfg, root)
        t0 = time.time()
        try:
            truth = sim.simulate(task, design, cfg["astfem_sim"],
                                 os.path.join(root, "sim"), env, log,
                                 cfg["sim_setup"], cfg["sim_timeout"])
        except Exception as exc:   # record it and go on with the next task
            for arm in todo:
                _write_json(result_path(outdir, task, arm), {
                    "task": task, "arm": arm, "status": "error",
                    "error": "simulation: %s: %s" % (type(exc).__name__,
                                                     exc),
                    "traceback": traceback.format_exc()})
                done.append((arm, "error"))
            return done
        sim_seconds = time.time() - t0
        for arm in todo:
            exe_key, wf = ARM_SPEC[arm]
            marker = result_path(outdir, task, arm)[:-5] + ".running"
            _write_json(marker, {"host": socket.gethostname(),
                                 "pid": os.getpid(), "start": time.time(),
                                 "job": os.environ.get("SLURM_JOB_ID"),
                                 "array_task":
                                     os.environ.get("SLURM_ARRAY_TASK_ID")})
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
            _write_json(result_path(outdir, task, arm), rec)
            _remove(marker)
            done.append((arm, rec["status"]))
    finally:
        if os.path.exists(log):
            shutil.copy(log, os.path.join(outdir, task["task"], "log.txt"))
        if not keep:
            shutil.rmtree(root, ignore_errors=True)
    return done
