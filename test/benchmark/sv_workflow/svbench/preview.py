"""Noise-free preview of every system x speed of a design.

For each combination the experiment is simulated once without any noise,
with the scan schedule and edit geometry the benchmark uses (reference
data-range end, geometry of the given replicate).  Output:

  OUT/<system>.png    scans of the system at every speed (every 5th scan,
                      data range marked), with each solute's observability
  OUT/summary.csv     per system x speed x solute:  scan times, boundary
                      positions, displacement, diffusion width, flags
  OUT/index.html      all figures and the table in one page
  OUT/raw/<system>/<speed>rpm/<runID>/
                      with --keep:  the raw noise-free simulator output
                      (AUC, time state, full-column edit)
"""

import base64
import csv
import html
import io
import os
import shutil
from concurrent.futures import ThreadPoolExecutor

from . import design as dsg
from . import observe, physics, sim, systems
from .runner import us3_env
from .util import read_auc


def _task(design, system, speed, range_end, replicate):
    return {"system": system, "speed": speed, "range_end": range_end,
            "ti_noise": 0, "ri_noise": 0, "random_noise": 0,
            "local_noise": 0, "baseline": 0, "replicate": replicate,
            "task": "prev_%s_%d" % (system.split("_")[0], speed),
            "noise_seed": 1,
            "geometry_seed": dsg._seed(design.get("base_seed", 1), system,
                                       speed, replicate)}


def simulate_one(design, cfg, task, root):
    """Simulate one noise-free combination; returns (data, truth)."""
    simp = design["simulation"]
    sols = systems.solutes(task["system"])
    (m_true, b_true, m_edit, b_edit, left, right, t1,
     t2) = sim.geometry(task, design)
    work = os.path.join(root, task["task"])
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    env = us3_env(cfg, work)
    sim.write_model(os.path.join(work, "model.xml"), sols,
                    simp["total_concentration"])
    sim.write_buffer(os.path.join(work, "buffer.xml"), simp)
    sim.write_simparams(os.path.join(work, "simparams.xml"), simp,
                        int(task["speed"]), t1, t2, task)
    out = sim.run_astfem_sim(
        cfg["astfem_sim"], work,
        os.path.join(work, "sim", "svbprev" + task["task"].split("_")[1]
                     + str(task["speed"])),
        1, simp["od_limit_factor"] * simp["total_concentration"], env,
        os.path.join(work, "log.txt"), cfg["sim_setup"], cfg["sim_timeout"])
    auc = [f for f in os.listdir(out) if f.endswith(".auc")][0]
    data = read_auc(os.path.join(out, auc))
    truth = {"meniscus": m_true, "bottom": b_true, "data_left": left,
             "data_right": right, "time_first": t1, "time_last": t2,
             "solutes": [dict(x, D=physics.coefficients(
                 x["s"], x["ff0"], x["vbar"])["D"]) for x in sols]}
    return data, truth


def _figure(system, panels):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    n = len(panels)
    cols = 3
    rows = (n + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(4.2 * cols, 3.0 * rows),
                             squeeze=False)
    for k, (speed, data, truth, obs) in enumerate(panels):
        ax = axes[k // cols][k % cols]
        r, t, v = data
        sel = (r >= truth["meniscus"] - 0.02)
        for j in range(0, len(t), 5):
            ax.plot(r[sel], v[j][sel], color="#2a78d6", lw=0.8,
                    alpha=0.35 + 0.65 * j / max(len(t) - 1, 1))
        for x in (truth["data_left"], truth["data_right"]):
            ax.axvline(x, color="#52514e", ls="--", lw=0.8)
        inr = (r >= truth["data_left"]) & (r <= truth["data_right"])
        ax.set_ylim(-0.05, max(1.15 * float(v[:, inr].max()), 0.2))
        ax.set_title("%d rpm   %.1f-%.1f h" % (
            speed, truth["time_first"] / 3600.0, truth["time_last"] / 3600.0),
            fontsize=9)
        lines = []
        for sol, ob in zip(truth["solutes"], obs):
            flag = ("ok" if ob["diff_ok"] else
                    "no D" if ob["sed_ok"] else "no sed")
            lines.append("%g S: move %.2f, diff %.3f  %s" % (
                sol["s"], ob["displacement"], ob["diffusion_width"], flag))
        ax.text(0.02, 0.97, "\n".join(lines), transform=ax.transAxes,
                fontsize=6.5, va="top", family="monospace")
        ax.tick_params(labelsize=7)
        ax.set_xlabel("radius (cm)", fontsize=7)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
    for k in range(n, rows * cols):
        axes[k // cols][k % cols].axis("off")
    fig.suptitle("%s  (%s)   every 5th scan, noise-free; dashed: data range"
                 % (system, systems.SYSTEMS[system]["description"]),
                 fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    return fig


def preview(design, cfg, outdir, range_end=None, replicate=0,
            systems_sel=None, speeds_sel=None, jobs=1, keep=False):
    os.makedirs(outdir, exist_ok=True)
    root = os.path.join(cfg["scratch"], "preview")
    os.makedirs(root, exist_ok=True)
    fac = design["factors"]
    sys_list = systems_sel or fac["system"]
    spd_list = speeds_sel or fac["speed"]
    rend = range_end if range_end is not None else \
        design["reference"]["range_end"]
    combos = [_task(design, s, int(v), rend, replicate)
              for s in sys_list for v in spd_list]

    def work(task):
        try:
            data, truth = simulate_one(design, cfg, task, root)
            obs = observe.solutes(truth, task["speed"],
                                  design["simulation"]["acceleration"])
            print("%-22s %6d  ok" % (task["system"], task["speed"]),
                  flush=True)
            return task, data, truth, obs, None
        except Exception as exc:
            print("%-22s %6d  FAILED: %s" % (task["system"], task["speed"],
                                            exc), flush=True)
            return task, None, None, None, str(exc)

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        results = list(pool.map(work, combos))

    rows = []
    figs = []
    for system in sys_list:
        panels = []
        for task, data, truth, obs, err in results:
            if task["system"] != system:
                continue
            if err:
                rows.append([system, task["speed"], "", "", "", "", "", "",
                             "", "", "", "simulation failed: " + err])
                continue
            panels.append((task["speed"], data, truth, obs))
            for sol, ob in zip(truth["solutes"], obs):
                rows.append([system, task["speed"], sol["s"], sol["ff0"],
                             truth["time_first"], truth["time_last"],
                             round(ob["boundary_first"], 4),
                             round(ob["boundary_last"], 4),
                             round(ob["displacement"], 4),
                             round(ob["diffusion_width"], 4),
                             ob["sed_ok"], ob["diff_ok"]])
        if panels:
            fig = _figure(system, panels)
            path = os.path.join(outdir, system + ".png")
            fig.savefig(path, dpi=110)
            buf = io.BytesIO()
            fig.savefig(buf, format="png", dpi=90)
            import matplotlib.pyplot as plt
            plt.close(fig)
            figs.append((system, base64.b64encode(buf.getvalue()).decode()))

    head = ["system", "speed", "s", "ff0", "time_first", "time_last",
            "boundary_first", "boundary_last", "displacement",
            "diffusion_width", "sed_ok", "diff_ok"]
    with open(os.path.join(outdir, "summary.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(head)
        w.writerows(rows)

    parts = ['<!doctype html><html lang="en"><head><meta charset="utf-8">'
             '<meta name="viewport" content="width=device-width,'
             'initial-scale=1"><title>SV design preview</title><style>'
             'body{font:14px system-ui,sans-serif;margin:16px;'
             'background:#fcfcfb;color:#0b0b0b}img{max-width:100%}'
             'table{border-collapse:collapse;font-size:12px}'
             'td,th{border-bottom:1px solid #e4e3df;padding:2px 8px}'
             '</style></head><body>',
             "<h1>Noise-free preview: %s</h1>" % html.escape(design["name"]),
             "<p>Data range end %.3g cm before the edit bottom, geometry of"
             " replicate %d. <b>move</b>: boundary displacement between first"
             " and last scan as fraction of the data range (sedimentation"
             " information if &ge; %.2f); <b>diff</b>: &radic;(2Dt) at the"
             " last scan as fraction of the data range (diffusion"
             " information if &ge; %.2f).</p>"
             % (rend, replicate, observe.MIN_DISPLACEMENT,
                observe.MIN_DIFFUSION_WIDTH)]
    for system, b64 in figs:
        parts.append('<h2>%s</h2><img alt="%s" src="data:image/png;base64,'
                     '%s">' % (system, system, b64))
    parts.append("<h2>Table</h2><table><tr>%s</tr>" % "".join(
        "<th>%s</th>" % h for h in head))
    for r in rows:
        parts.append("<tr>%s</tr>" % "".join(
            "<td>%s</td>" % html.escape(str(c)) for c in r))
    parts.append("</table></body></html>")
    with open(os.path.join(outdir, "index.html"), "w") as fh:
        fh.write("\n".join(parts))
    if keep:
        # Raw simulator output (AUC, time state, the simulator's own
        # full-column edit) as OUT/raw/<system>/<speed>rpm/<runID>/
        dest = os.path.join(outdir, "raw")
        if os.path.exists(dest):
            shutil.rmtree(dest)
        for task, data, truth, obs, err in results:
            if err:
                continue
            src = os.path.join(root, task["task"], "sim")
            for run in os.listdir(src):
                shutil.copytree(os.path.join(src, run), os.path.join(
                    dest, task["system"], "%drpm" % task["speed"], run))
    shutil.rmtree(root, ignore_errors=True)
    nfail = sum(1 for x in results if x[4])
    print("%d combinations, %d failed -> %s"
          % (len(results), nfail, os.path.join(outdir, "index.html")))
    return 1 if nfail else 0
