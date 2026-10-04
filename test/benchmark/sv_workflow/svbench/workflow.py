"""The two SV analysis workflows, run with one us_mpi_analysis build.

old workflow:
  1. 2DSA, TI noise only                        -> TI noise N1
  2. 2DSA meniscus+bottom fit, TI+RI, N1 loaded
  3. fit-meniscus:  update meniscus/bottom of the edit
  4. 2DSA iterative refinement, TI+RI, N1 loaded (analysis.old_step3_noise)
new workflow:
  steps 2-4 without step 1 and without loaded noise.

Noise files written by us_mpi_analysis include the loaded noise, so the
final TI noise of the old workflow is N1 plus the TI noise fitted in step 4.
"""

import os
import shutil

from . import fitmen, mpi, systems


def s_range(task, ana):
    sv = [x["s"] for x in systems.solutes(task["system"])]
    return (min(sv) * ana["s_range_factor_low"],
            max(sv) * ana["s_range_factor_high"])


def _noise_files(model, types=("ti", "ri")):
    return [n["file"] for n in model["noises"] if n["type"] in types]


def _summary(res):
    return {"wall_seconds": res["wall_seconds"],
            "models": len(res["models"])}


def run_arm(task, design, truth, data_dir, exe, mpirun, workdir, log,
            workflow, timeout=None):
    ana = design["analysis"]
    files = truth["files"]
    srng = s_range(task, ana)
    vbar = systems.VBAR
    out = {"workflow": workflow, "steps": {}}
    os.makedirs(workdir, exist_ok=True)

    def job(name, params, noise, ddir):
        return mpi.run(exe, mpirun, os.path.join(workdir, name), ddir,
                       files, noise, params, design, vbar, log, timeout)

    # Step 1 (old workflow only):  2DSA with TI noise
    noise_in = []
    if workflow == "old":
        r1 = job("step1_2dsa_ti",
                 mpi.job_parameters(ana, srng, ti=True, ri=False), [],
                 data_dir)
        out["steps"]["step1"] = _summary(r1)
        out["steps"]["step1"]["rmsd"] = r1["models"][-1]["variance"] ** 0.5
        noise_in = _noise_files(r1["models"][-1], ("ti",))

    # Step 2:  meniscus + bottom fit with TI and RI noise
    r2 = job("step2_fit_mb",
             mpi.job_parameters(ana, srng, ti=True, ri=True, fit_mb=True),
             noise_in, data_dir)
    out["steps"]["step2"] = _summary(r2)
    pts = fitmen.points_from_models(r2["models"])
    fm = fitmen.fit_meniscus_bottom(pts)
    out["fit_mb"] = fm
    out["fit_mb_grid"] = pts

    # Step 3:  fit-meniscus edit update
    upd_dir = os.path.join(workdir, "data_updated")
    if os.path.exists(upd_dir):
        shutil.rmtree(upd_dir)
    shutil.copytree(data_dir, upd_dir)
    edit = files["edit"]
    ok = fitmen.update_edit(os.path.join(data_dir, edit),
                            os.path.join(upd_dir, edit),
                            fm["meniscus"], fm["bottom"])
    out["edit_updated"] = ok

    # Step 4:  iterative refinement with TI and RI noise
    noise3 = []
    if workflow == "old":
        mode = ana.get("old_step3_noise", "step1")
        if mode == "step1":
            noise3 = noise_in
        elif mode == "mbbest":
            best = min(r2["models"], key=lambda m: m["variance"])
            noise3 = _noise_files(best)
        elif mode != "none":
            raise ValueError("analysis.old_step3_noise: " + mode)
    r3 = job("step4_2dsa_ir",
             mpi.job_parameters(ana, srng, ti=True, ri=True,
                                max_iter=ana["ir_max_iterations"]),
             noise3, upd_dir)
    out["steps"]["step4"] = _summary(r3)
    final = r3["models"][-1]
    out["final"] = {
        "variance": final["variance"],
        "rmsd": final["variance"] ** 0.5,
        "meniscus": final["meniscus"],
        "bottom": final["bottom"],
        "components": final["components"],
        "noises": [{k: n[k] for k in ("type", "minradius", "maxradius",
                                      "values")}
                   for n in final["noises"]],
    }
    out["wall_seconds"] = sum(s["wall_seconds"]
                              for s in out["steps"].values())
    return out
