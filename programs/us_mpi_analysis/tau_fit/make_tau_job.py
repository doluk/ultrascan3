#!/usr/bin/env python3
"""Build a us_mpi_analysis job archive (hpcrequest XML + data files) for a
2DSA that fits the meniscus and the radial scan time (tau) together.

The archive is written in the format the UltraScan LIMS submits, so it runs
with the standard command

    mpirun -np <N> us_mpi_analysis -walltime <minutes> -mgroupcount 1 <job>.tar

Example (XL-A data, scan from the first raw radius outward):

    make_tau_job.py --auc run.RA.1.A.280.auc --edit run.e1.RA.1.A.280.xml \\
        --density 0.99823 --viscosity 1.002 --vbar 0.733 \\
        --centerpiece-bottom 7.2 --rotor-stretch "0 1e-12" \\
        --s-min 1 --s-max 10 --ff0-min 1 --ff0-max 4 \\
        --meniscus-range 0.02 --meniscus-points 11 \\
        --tau-min 0 --tau-max 0.1 --tau-points 11 \\
        --ti-noise --ri-noise -o run_tau.tar

Several datasets can be given by repeating --auc/--edit; they are fitted one
after the other (a composite job), each with the same meniscus/tau grid.
"""

import argparse
import re
import shutil
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


def sub(parent, tag, **attrs):
    return ET.SubElement(parent, tag, {k: str(v) for k, v in attrs.items()})


def job_parameters(a):
    p = {
        "s_min": a.s_min, "s_max": a.s_max, "s_grid_points": a.s_points,
        "ff0_min": a.ff0_min, "ff0_max": a.ff0_max, "ff0_grid_points": a.ff0_points,
        "uniform_grid": a.uniform_grid,
        "max_iterations": a.max_iterations, "mc_iterations": 1,
        "tinoise_option": 1 if a.ti_noise else 0,
        "rinoise_option": 1 if a.ri_noise else 0,
        "debug_level": 0, "seed": a.seed,
        "scan_direction": a.scan_direction,
    }
    if a.meniscus_points > 1:
        p.update(meniscus_points=a.meniscus_points, meniscus_range=a.meniscus_range,
                 fit_mb_select=1)
    else:
        p.update(meniscus_points=1, meniscus_range=0.0, fit_mb_select=0)
    if a.tau_points > 1:
        p.update(tau_points=a.tau_points, tau_min=a.tau_min, tau_max=a.tau_max)
    else:
        p.update(radial_scan_time=a.radial_scan_time)
    if a.scan_start > 0.0:
        p["scan_start"] = a.scan_start
    return p


def job_xml(a, datasets):
    root = ET.Element("US_JobSubmit", method="2DSA", version="1.0")
    job = sub(root, "job")
    sub(job, "cluster", name="local", shortname="local")
    sub(job, "name", value="us3_tau_fit")
    # No status messages (empty port): the run reports to its log only
    sub(job, "udp", server="127.0.0.1", port="")
    sub(job, "global_fit", value="0")
    sub(job, "request", id=str(a.request_id), guid="00000000-0000-4000-8000-%012d" % a.request_id)
    params = sub(job, "jobParameters")
    for key, value in job_parameters(a).items():
        sub(params, key, value=value)

    for ds in datasets:
        dset = sub(root, "dataset")
        files = sub(dset, "files")
        sub(files, "auc", filename=ds["auc"].name)
        sub(files, "edit", filename=ds["edit"].name)
        for noise in ds["noise"]:
            sub(files, "noise", filename=noise.name)
        if ds["timestate"]:
            sub(files, "timestate", filename=ds["timestate"].name)
        solution = sub(dset, "solution")
        sub(solution, "buffer", density=a.density, viscosity=a.viscosity,
            compress=a.compressibility, manual="1" if a.manual else "0")
        sub(solution, "analyte", mw=a.mw, vbar20=a.vbar, amount="1", type=a.analyte_type)
        sub(dset, "band_volume", value=a.band_volume)
        sub(dset, "rotor_stretch", value=a.rotor_stretch)
        sub(dset, "centerpiece_bottom", value=a.centerpiece_bottom)
        sub(dset, "centerpiece_shape", value=a.centerpiece_shape)
        sub(dset, "centerpiece_angle", value=a.centerpiece_angle)
        sub(dset, "centerpiece_pathlength", value=a.centerpiece_pathlength)
        sub(dset, "centerpiece_width", value=a.centerpiece_width)
        sub(dset, "total_concentration", value=a.total_concentration)

    if hasattr(ET, "indent"):  # Python 3.9+
        ET.indent(root)
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    d = ap.add_argument_group("data (repeat --auc/--edit for several datasets)")
    d.add_argument("--auc", action="append", required=True, help="raw data file (.auc)")
    d.add_argument("--edit", action="append", required=True, help="edit profile (.xml) of the --auc at the same position")
    d.add_argument("--noise", action="append", default=[],
                   help="noise file to subtract before the fit, as dataset:file (dataset counted from 1) or file for the first")
    d.add_argument("--timestate", action="append", default=[],
                   help="timestate .tmst of the run (its .xml is taken along); one per dataset or one for all")
    s = ap.add_argument_group("solution and hardware")
    s.add_argument("--density", type=float, required=True, help="buffer density (g/ml)")
    s.add_argument("--viscosity", type=float, required=True, help="buffer viscosity (cP)")
    s.add_argument("--compressibility", type=float, default=0.0)
    s.add_argument("--manual", action="store_true", help="density/viscosity entered manually (no temperature correction)")
    s.add_argument("--vbar", type=float, required=True, help="analyte vbar at 20 C (ml/g)")
    s.add_argument("--mw", type=float, default=50000.0, help="analyte MW (only used for the record)")
    s.add_argument("--analyte-type", default="Protein", choices=["Protein", "DNA", "RNA", "Carbohydrate"])
    s.add_argument("--centerpiece-bottom", type=float, required=True, help="channel bottom at rest (cm)")
    s.add_argument("--rotor-stretch", required=True, help='rotor stretch coefficients, e.g. "0 1e-12"')
    s.add_argument("--centerpiece-shape", default="standard")
    s.add_argument("--centerpiece-angle", type=float, default=2.5)
    s.add_argument("--centerpiece-pathlength", type=float, default=1.2)
    s.add_argument("--centerpiece-width", type=float, default=0.0)
    s.add_argument("--band-volume", type=float, default=0.0, help="lamella volume (ml) for band forming; 0: boundary run")
    s.add_argument("--total-concentration", type=float, default=1.0)
    g = ap.add_argument_group("2DSA grid")
    g.add_argument("--s-min", type=float, required=True)
    g.add_argument("--s-max", type=float, required=True)
    g.add_argument("--s-points", type=int, default=64)
    g.add_argument("--ff0-min", type=float, required=True)
    g.add_argument("--ff0-max", type=float, required=True)
    g.add_argument("--ff0-points", type=int, default=64)
    g.add_argument("--uniform-grid", type=int, default=6, help="subgrid refinements per dimension")
    g.add_argument("--max-iterations", type=int, default=1, help="iterative refinement passes")
    g.add_argument("--ti-noise", action="store_true", help="fit time-invariant noise")
    g.add_argument("--ri-noise", action="store_true", help="fit radially invariant noise")
    g.add_argument("--seed", type=int, default=0)
    t = ap.add_argument_group("meniscus and radial scan time")
    t.add_argument("--meniscus-range", type=float, default=0.0, help="meniscus range (cm), centred on the edited meniscus")
    t.add_argument("--meniscus-points", type=int, default=1)
    t.add_argument("--tau-min", type=float, default=0.0, help="lowest radial scan time (s/point)")
    t.add_argument("--tau-max", type=float, default=0.0, help="highest radial scan time (s/point)")
    t.add_argument("--tau-points", type=int, default=1, help="radial scan time points; > 1 fits tau")
    t.add_argument("--radial-scan-time", type=float, default=0.0, help="fixed radial scan time (s/point) when tau is not fitted")
    t.add_argument("--scan-start", type=float, default=0.0,
                   help="radius (cm) where each scan starts; 0: first raw radius in scan direction")
    t.add_argument("--scan-direction", default="outward", choices=["outward", "inward"])
    ap.add_argument("--request-id", type=int, default=1)
    ap.add_argument("-o", "--output", required=True, help="job archive to write (.tar)")
    a = ap.parse_args()

    if len(a.auc) != len(a.edit):
        ap.error("give one --edit for every --auc")
    if a.tau_points > 1 and not a.tau_max > a.tau_min:
        ap.error("--tau-max must be larger than --tau-min")
    if a.meniscus_points > 1 and a.meniscus_range <= 0.0:
        ap.error("--meniscus-range must be positive with --meniscus-points > 1")

    datasets = []
    for ii, (auc, edit) in enumerate(zip(a.auc, a.edit)):
        datasets.append({"auc": Path(auc), "edit": Path(edit), "noise": [], "timestate": None})
    for spec in a.noise:
        m = re.match(r"^(\d+):(.+)$", spec)
        idx, name = (int(m.group(1)), m.group(2)) if m else (1, spec)
        if not 1 <= idx <= len(datasets):
            ap.error("--noise %s: no dataset %d" % (spec, idx))
        datasets[idx - 1]["noise"].append(Path(name))
    for ii, tmst in enumerate(a.timestate):
        targets = datasets if len(a.timestate) == 1 else [datasets[ii]]
        for ds in targets:
            ds["timestate"] = Path(tmst)

    files = set()
    for ds in datasets:
        files |= {ds["auc"], ds["edit"], *ds["noise"]}
        if ds["timestate"]:
            files.add(ds["timestate"])
            xml = ds["timestate"].with_suffix(".xml")
            if xml.exists():
                files.add(xml)
    missing = [str(f) for f in files if not f.exists()]
    if missing:
        ap.error("missing file(s): " + ", ".join(missing))
    names = [f.name for f in files]
    if len(set(names)) != len(names):
        ap.error("the archive is flat: input files must have distinct names")

    with tempfile.TemporaryDirectory() as tmp:
        xml_path = Path(tmp) / ("hpcrequest-local-tau-%d.xml" % a.request_id)
        xml_path.write_text(job_xml(a, datasets))
        with tarfile.open(a.output, "w") as tar:
            tar.add(xml_path, arcname=xml_path.name)
            for f in sorted(files):
                tar.add(f, arcname=f.name)
        shutil.copy(xml_path, Path(a.output).with_suffix(".xml"))

    npts = max(1, a.meniscus_points) * max(1, a.tau_points)
    print("wrote %s: %d dataset(s), %d fit point(s) per dataset (meniscus %d x tau %d)"
          % (a.output, len(datasets), npts, max(1, a.meniscus_points), max(1, a.tau_points)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
