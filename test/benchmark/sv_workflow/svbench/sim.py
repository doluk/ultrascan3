"""Simulation of one task with us_astfem_sim and preparation of its edit.

us_astfem_sim writes the AUC data, an edit (full column, perfect meniscus
and bottom), the time state and the true TI/RI noise as CSV.  The edit is
then rewritten so that its meniscus and bottom are off by a random amount
(as from a manual edit) and the data range ends a given distance before
the bottom, which leaves room for a real meniscus and bottom fit.

Each task is simulated twice with the same schedule:  with noise, and
without any noise; the difference of the two holds the true TI, RI,
baseline and random noise.
"""

import glob
import os
import random
import re
import shutil
import subprocess
import uuid
import xml.etree.ElementTree as ET

from . import physics, systems
from .util import run_logged


def _guid():
    return str(uuid.uuid4())


def write_model(path, solutes, total_conc):
    comps = []
    for i, sol in enumerate(solutes):
        c = physics.coefficients(sol["s"], sol["ff0"], sol["vbar"])
        comps.append(
            '    <analyte name="SC%04d" mw="%.6e" s="%.6e" D="%.6e" f="%.6e"'
            ' f_f0="%.6f" vbar20="%.4f" extinction="1" axial="10" sigma="0"'
            ' delta="0" oligomer="1" shape="2" type="0" molar="0"'
            ' signal="%.8f"/>'
            % (i + 1, c["mw"], c["s"], c["D"], c["f"], sol["ff0"],
               sol["vbar"], sol["fraction"] * total_conc))
    text = ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE US_Model>\n<ModelData version="1.0">\n'
            '  <model description="svbench_truth" modelGUID="%s"'
            ' editGUID="00000000-0000-0000-0000-000000000000"'
            ' wavelength="280" coSedSolute="-1" opticsType="0"'
            ' analysisType="0" globalType="0">\n%s\n  </model>\n'
            '</ModelData>\n' % (_guid(), "\n".join(comps)))
    with open(path, "w") as fh:
        fh.write(text)


def write_buffer(path, sim):
    text = ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE US_Buffer>\n<BufferData version="1.0">\n'
            '  <buffer id="0" guid="%s" description="svbench water"'
            ' ph="7.00000" density="%.6f" viscosity="%.5f"'
            ' compressibility="0" manual="1">\n'
            '    <spectrum/>\n  </buffer>\n</BufferData>\n'
            % (_guid(), sim["buffer_density"], sim["buffer_viscosity"]))
    with open(path, "w") as fh:
        fh.write(text)


def write_simparams(path, sim, rpm, t_first, t_last, noise):
    delay_min = t_first / 60.0
    dur_h = int(t_last // 3600)
    dur_min = (t_last - dur_h * 3600) / 60.0
    text = ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE US_SimParams>\n<SimParams version="1.0">\n'
            '  <params meshType="%s" gridType="%s" simpoints="%d"'
            ' radialres="%g" meniscus="%.6f" bottom="%.6f" rnoise="%g"'
            ' lrnoise="%g" tinoise="%g" rinoise="%g" baseline="%g"'
            ' temperature="%g" bandform="0" sector="0" pathlength="1.2"'
            ' angle="2.5" width="0">\n'
            '    <speedstep rotorspeed="%d" scans="%d" duration_hrs="%d"'
            ' duration_mins="%.6f" delay_hrs="0" delay_mins="%.6f"'
            ' acceleration="%d" accelerflag="1"/>\n'
            '  </params>\n</SimParams>\n'
            % (sim["mesh"], sim["grid"], sim["simpoints"],
               sim["radial_resolution"], sim["meniscus"], sim["bottom"],
               noise["random_noise"], noise["local_noise"],
               noise["ti_noise"], noise["ri_noise"], noise["baseline"],
               sim["temperature"], rpm, sim["scans"], dur_h, dur_min,
               delay_min, sim["acceleration"]))
    with open(path, "w") as fh:
        fh.write(text)


def run_astfem_sim(exe, workdir, outdir, seed, od_limit, env, log,
                   setup=None, timeout=900):
    """Run us_astfem_sim headless; the run ID is the basename of outdir."""
    os.makedirs(outdir, exist_ok=True)
    cmd = [exe, "--model", os.path.join(workdir, "model.xml"),
           "--buffer", os.path.join(workdir, "buffer.xml"),
           "--simparams", os.path.join(workdir, "simparams.xml"),
           "--rotor", "0:0", "--seed", str(seed),
           "--odlimit", "%g" % od_limit, "--no-db",
           "--start", "--save", outdir.rstrip("/"), "--close", "--errors-cl"]
    run_logged(cmd, log, env=env, cwd=workdir, timeout=timeout,
               setup=setup)
    aucs = glob.glob(os.path.join(outdir, "*.auc"))
    if len(aucs) != 1:
        raise RuntimeError("us_astfem_sim: expected one .auc in %s, got %s"
                           % (outdir, aucs))
    return outdir


def _read_csv(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            parts = [p.strip().strip('"') for p in line.split(",")]
            try:
                rows.append([float(p) for p in parts[:2]])
            except ValueError:
                continue          # header
    return rows


def rewrite_edit(path, meniscus, bottom, left, right):
    """Set meniscus, bottom and data range of an edit file in place."""
    tree = ET.parse(path)
    par = tree.getroot().find("./run/parameters")
    par.find("meniscus").set("radius", "%.8f" % meniscus)
    par.find("bottom").set("radius", "%.8f" % bottom)
    rng = par.find("data_range")
    rng.set("left", "%.8f" % left)
    rng.set("right", "%.8f" % right)
    par.find("plateau").set("radius", "%.8f" % (right - 0.01))
    par.find("baseline").set("radius", "%.8f" % (left + 0.005))
    with open(path) as fh:
        head = fh.read().split("<experiment", 1)[0]
    body = ET.tostring(tree.getroot(), encoding="unicode")
    with open(path, "w") as fh:
        fh.write(head + body + "\n")


def geometry(task, design):
    """Edit geometry (random meniscus/bottom error, data range) and scan
    schedule of a task."""
    sim = design["simulation"]
    edt = design["edit"]
    rng = random.Random(task["geometry_seed"])
    m_true, b_true = sim["meniscus"], sim["bottom"]
    m_edit = m_true + rng.uniform(-1, 1) * edt["meniscus_offset_max"]
    b_edit = b_true + rng.uniform(-1, 1) * edt["bottom_offset_max"]
    left = m_edit + edt["data_left_gap"]
    right = b_edit - task["range_end"]
    t_first, t_last = physics.scan_schedule(
        systems.solutes(task["system"]), int(task["speed"]), m_true, right,
        sim)
    return m_true, b_true, m_edit, b_edit, left, right, t_first, t_last


def simulate(task, design, exe, workdir, env, log, setup=None,
             timeout=900):
    """Simulate one task; returns the truth record (dict)."""
    sim = design["simulation"]
    sols = systems.solutes(task["system"])
    rpm = int(task["speed"])
    (m_true, b_true, m_edit, b_edit, left, right, t_first,
     t_last) = geometry(task, design)

    os.makedirs(workdir, exist_ok=True)
    write_model(os.path.join(workdir, "model.xml"), sols,
                sim["total_concentration"])
    write_buffer(os.path.join(workdir, "buffer.xml"), sim)

    od_limit = sim["od_limit_factor"] * sim["total_concentration"]
    run_id = "svb" + task["task"].replace("_", "")
    out = {}
    zero = {k: 0 for k in ("random_noise", "local_noise", "ti_noise",
                           "ri_noise", "baseline")}
    for kind, noise in (("clean", zero), ("noisy", task)):
        write_simparams(os.path.join(workdir, "simparams.xml"), sim, rpm,
                        t_first, t_last, noise)
        raw = os.path.join(workdir, "sim_" + kind, run_id)
        out[kind] = run_astfem_sim(exe, workdir, raw, task["noise_seed"],
                                   od_limit, env, log, setup, timeout)

    # Data set used by the analyses:  noisy AUC, time state and edit
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir, exist_ok=True)
    files = {}
    for src in sorted(glob.glob(os.path.join(out["noisy"], "*"))):
        name = os.path.basename(src)
        if name.endswith(".auc"):
            files["auc"] = name
        elif name.endswith(".time_state.tmst"):
            files["tmst"] = name
        elif re.search(r"\.\d{10}\.[A-Z]{2}\.\d+\.[A-Z]\.\d+\.xml$", name):
            files["edit"] = name
        elif not name.endswith(".xml"):
            continue
        shutil.copy(src, data_dir)
    if "edit" not in files or "auc" not in files:
        raise RuntimeError("simulation output incomplete: %s" % files)
    rewrite_edit(os.path.join(data_dir, files["edit"]), m_edit, b_edit,
                 left, right)

    # True noise components
    ti_csv = glob.glob(os.path.join(out["noisy"], "*TI_NOISE*.csv"))
    ri_csv = glob.glob(os.path.join(out["noisy"], "*RI_NOISE*.csv"))
    truth = {
        "meniscus": m_true, "bottom": b_true,
        "edit_meniscus": m_edit, "edit_bottom": b_edit,
        "data_left": left, "data_right": right,
        "time_first": t_first, "time_last": t_last,
        "solutes": [dict(x, conc=x["fraction"] * sim["total_concentration"],
                         **{k: v for k, v in physics.coefficients(
                             x["s"], x["ff0"], x["vbar"]).items()
                            if k in ("D", "mw")})
                    for x in sols],
        "total_concentration": sim["total_concentration"],
        "baseline": task["baseline"],
        "ti_noise": _read_csv(ti_csv[0]) if ti_csv else [],
        "ri_noise": _read_csv(ri_csv[0]) if ri_csv else [],
        "files": files,
        "data_dir": data_dir,
        "clean_auc": glob.glob(os.path.join(out["clean"], "*.auc"))[0],
        "noisy_auc": glob.glob(os.path.join(out["noisy"], "*.auc"))[0],
    }
    return truth
