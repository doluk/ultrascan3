"""Run a 2DSA job with us_mpi_analysis, as the LIMS would submit it.

A job is a tar file holding the hpcrequest XML and the data, edit,
time state and (optionally) noise files.  us_mpi_analysis writes its
results to output/analysis-results.tar:  one model per fit (one per
meniscus/bottom grid point for a meniscus+bottom fit), the TI/RI noise of
each model (input noise added in), and analysis_files.txt that lists the
models with meniscus, bottom and variance.
"""

import os
import shlex
import shutil
import tarfile
import time
import uuid
import xml.etree.ElementTree as ET

from .util import run_logged


def _param(name, value):
    return '      <%s value="%s"/>\n' % (name, value)


def write_request(path, params, files, ana, sim, vbar):
    jp = "".join(_param(k, v) for k, v in params.items())
    fl = ['        <auc filename="%s"/>' % files["auc"],
          '        <edit filename="%s"/>' % files["edit"]]
    if files.get("tmst"):
        fl.append('        <timestate filename="%s"/>' % files["tmst"])
    for nf in files.get("noise", []):
        fl.append('        <noise filename="%s"/>' % nf)
    text = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<US_JobSubmit method="2DSA" version="1.0">\n'
        '  <job>\n'
        '    <cluster name="svbench" shortname="svbench"/>\n'
        '    <name value="svbench"/>\n'
        '    <udp server="" port="0"/>\n'
        '    <request id="1" guid="%s"/>\n'
        '    <jobParameters>\n%s    </jobParameters>\n'
        '  </job>\n'
        '  <dataset>\n'
        '    <files>\n%s\n    </files>\n'
        '    <solution>\n'
        '      <buffer density="%.6f" viscosity="%.5f" compress="0"'
        ' manual="1"/>\n'
        '      <analyte mw="50000" vbar20="%.4f" amount="1"'
        ' type="Protein"/>\n'
        '    </solution>\n'
        '    <simpoints value="%d"/>\n'
        '    <band_volume value="0"/>\n'
        '    <radial_grid value="%d"/>\n'
        '    <time_grid value="%d"/>\n'
        '    <rotor_stretch value="0 0"/>\n'
        '    <centerpiece_bottom value="%.6f"/>\n'
        '    <centerpiece_shape value="sector"/>\n'
        '    <centerpiece_angle value="2.5"/>\n'
        '    <centerpiece_pathlength value="1.2"/>\n'
        '    <centerpiece_width value="0"/>\n'
        '    <total_concentration value="%g"/>\n'
        '  </dataset>\n'
        '</US_JobSubmit>\n'
        % (uuid.uuid4(), jp, "\n".join(fl), sim["buffer_density"],
           sim["buffer_viscosity"], vbar, ana["simpoints"],
           ana["radial_grid"], ana["time_grid"], sim["bottom"],
           sim["total_concentration"]))
    with open(path, "w") as fh:
        fh.write(text)


def job_parameters(ana, s_range, ti, ri, max_iter=1, fit_mb=False):
    p = {
        "s_min": "%.6g" % s_range[0],
        "s_max": "%.6g" % s_range[1],
        "ff0_min": ana["ff0_min"],
        "ff0_max": ana["ff0_max"],
        "s_grid_points": ana["s_grid_points"],
        "ff0_grid_points": ana["ff0_grid_points"],
        "uniform_grid": ana["uniform_grid"],
        "mc_iterations": 1,
        "tinoise_option": 1 if ti else 0,
        "rinoise_option": 1 if ri else 0,
        "max_iterations": max_iter,
        "regularization": 0,
        "debug_level": 0,
    }
    if fit_mb:
        p.update({"fit_mb_select": 3,
                  "meniscus_range": ana["meniscus_range"],
                  "meniscus_points": ana["meniscus_points"]})
    else:
        p.update({"fit_mb_select": 0, "meniscus_range": 0,
                  "meniscus_points": 1})
    return p


def run(exe, mpirun, jobdir, data_dir, files, noise_files, params, design,
        vbar, log, timeout=None, setup=None):
    """Run one us_mpi_analysis job in jobdir; returns parsed results."""
    if os.path.exists(jobdir):
        shutil.rmtree(jobdir)
    os.makedirs(jobdir)
    names = []
    for key in ("auc", "edit", "tmst"):
        if files.get(key):
            shutil.copy(os.path.join(data_dir, files[key]), jobdir)
            names.append(files[key])
    tdef = files.get("tmst", "").replace(".tmst", ".xml")
    if tdef and os.path.exists(os.path.join(data_dir, tdef)):
        shutil.copy(os.path.join(data_dir, tdef), jobdir)
        names.append(tdef)
    nnames = []
    for i, nf in enumerate(noise_files):
        name = "input_%d.%s" % (i, os.path.basename(nf))
        shutil.copy(nf, os.path.join(jobdir, name))
        nnames.append(name)
    names += nnames
    write_request(os.path.join(jobdir, "hpcrequest-svbench.xml"), params,
                  dict(files, noise=nnames), design["analysis"],
                  design["simulation"], vbar)
    names.append("hpcrequest-svbench.xml")
    with tarfile.open(os.path.join(jobdir, "job.tar"), "w") as tar:
        for n in names:
            tar.add(os.path.join(jobdir, n), arcname=n)
    for n in names:
        os.remove(os.path.join(jobdir, n))

    cmd = shlex.split(mpirun) + [exe, "job.tar"]
    t0 = time.time()
    run_logged(cmd, log, cwd=jobdir, timeout=timeout, setup=setup)
    wall = time.time() - t0

    res = os.path.join(jobdir, "output", "analysis-results.tar")
    outdir = os.path.join(jobdir, "results")
    os.makedirs(outdir)
    with tarfile.open(res) as tar:
        tar.extractall(outdir)
    parsed = parse_results(outdir)
    parsed["wall_seconds"] = wall
    return parsed


def parse_results(outdir):
    """Models (with meniscus/bottom/variance) and noises of a job."""
    models = []
    noises = []
    with open(os.path.join(outdir, "analysis_files.txt")) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            parts = line.split(";")
            if parts[0].endswith(".model.xml"):
                rec = {"file": os.path.join(outdir, parts[0])}
                for kv in parts[1:]:
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        rec[k] = v
                rec["meniscus"] = float(rec["meniscus_value"])
                rec["bottom"] = float(rec.get("bottom_value", 0) or 0)
                rec["variance"] = float(rec["variance"])
                models.append(rec)
            elif ".noise." in parts[0]:
                noises.append(os.path.join(outdir, parts[0]))
    for m in models:
        m.update(read_model(m["file"]))
    noise_recs = [read_noise(n) for n in noises]
    for m in models:
        m["noises"] = [n for n in noise_recs
                       if n["modelGUID"] == m["modelGUID"]]
    return {"models": models}


def read_model(path):
    root = ET.parse(path).getroot()
    mod = root.find("model")
    comps = []
    for an in mod.findall("analyte"):
        comps.append({"s": float(an.get("s")) * 1e13,
                      "ff0": float(an.get("f_f0")),
                      "D": float(an.get("D")),
                      "mw": float(an.get("mw")),
                      "conc": float(an.get("signal", 0))})
    return {"modelGUID": mod.get("modelGUID"),
            "model_meniscus": float(mod.get("meniscus", 0) or 0),
            "model_bottom": float(mod.get("bottom", 0) or 0),
            "components": comps}


def read_noise(path):
    root = ET.parse(path).getroot()
    nz = root.find("noise")
    vals = [float(d.get("v")) for d in nz.findall("d")]
    return {"file": path, "type": nz.get("type"),
            "modelGUID": nz.get("modelGUID"),
            "minradius": float(nz.get("minradius", 0) or 0),
            "maxradius": float(nz.get("maxradius", 0) or 0),
            "values": vals}
