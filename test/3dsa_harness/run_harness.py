#!/usr/bin/env python3
"""Drive the 3DSA test harness end to end.

Four stages:

  1. ``us_3dsa_cli gen-cases`` writes the case tree -- model, buffer and
     simulation-parameter XML for every data set of every case, plus a
     manifest naming the simulator runs.
  2. ``us_astfem_sim`` is run once per data set, from the manifest, to produce
     the .auc data.
  3. The run each simulation registered is *prepared for analysis*: the edit
     profile's radial range is widened away from the meniscus, and the analyte
     and solution records have their partial specific volume replaced with a
     decoy.  See ``prepare_run`` below for why both are necessary.
  4. ``us_3dsa_cli fit`` is run once per case and reports the global RMSD and
     the RMSD of every data set.

The .auc file name us_astfem_sim chooses depends on the optical type, cell,
channel and wavelength it settles on, so the driver discovers the file it
actually wrote rather than predicting it, and patches the case file before
fitting.

us_astfem_sim registers analytes, a buffer and a solution under
``$HOME/ultrascan/data`` so the run loads as though it had come through
us_convert and us_edit.  The driver points HOME at a directory inside
``--outdir`` so a harness run neither reads nor disturbs a real UltraScan
installation, and so repeated runs start from a clean store.

Examples
--------

    ./run_harness.py --outdir /tmp/3dsa --bindir build/hpc/bin
    ./run_harness.py --outdir /tmp/3dsa --only 'case1*' --jobs 4
    ./run_harness.py --outdir /tmp/3dsa --skip-sim      # refit existing data
"""

import argparse
import concurrent.futures as futures
import fnmatch
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

# us_astfem_sim links the Qt GUI stack even when driven headless, so it needs
# a platform plugin that does not want a display.  HOME is added per run, in
# main(), once --outdir is known.
HEADLESS_ENV = dict(os.environ, QT_QPA_PLATFORM="offscreen")


def find_binary(explicit, bindir, name):
    """Locate a binary from an explicit path, a build directory, or PATH."""
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit(f"{name}: not found at {explicit}")
        return os.path.abspath(explicit)

    if bindir:
        candidate = os.path.join(bindir, name)
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)

    found = shutil.which(name)
    if found:
        return found

    sys.exit(
        f"{name}: not found. Pass --bindir <dir with {name}> or "
        f"--{name.replace('_', '-')} <path>."
    )


def run(cmd, timeout, label):
    """Run a command, returning (ok, stdout+stderr, seconds)."""
    started = time.time()
    try:
        proc = subprocess.run(
            cmd,
            env=HEADLESS_ENV,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, f"{label}: timed out after {timeout} s", time.time() - started
    except OSError as exc:
        return False, f"{label}: {exc}", time.time() - started

    output = proc.stdout.decode("utf-8", "replace")
    return proc.returncode == 0, output, time.time() - started


def simulate_one(step, astfem_sim, timeout):
    """Run us_astfem_sim for one data set and return the .auc it produced."""
    outdir = step["outdir"]
    os.makedirs(outdir, exist_ok=True)

    # Clear stale output so the discovery below cannot pick up an old file.
    for stale in glob.glob(os.path.join(outdir, "*.auc")):
        os.remove(stale)

    cmd = [
        astfem_sim,
        "--model", step["model"],
        "--buffer", step["buffer"],
        "--simparams", step["simparams"],
        "--save", outdir,
        "--start",
        "--close",
        "--no-db",
        "--errors-cl",
    ]

    ok, output, secs = run(cmd, timeout, step["runid"])

    produced = sorted(glob.glob(os.path.join(outdir, "*.auc")))

    if not produced:
        return None, f"{step['runid']}: no .auc written\n{output}", secs
    if len(produced) > 1:
        return None, (
            f"{step['runid']}: {len(produced)} .auc files written, expected 1: "
            f"{produced}"
        ), secs
    if not ok:
        # A non-zero exit with data present is worth reporting but not fatal:
        # the GUI program returns its init status, not a simulation status.
        print(f"  note: {step['runid']} exited non-zero but wrote data")

    return produced[0], output, secs


# ---------------------------------------------------------------------------
# Preparing a simulated run for analysis
# ---------------------------------------------------------------------------
#
# us_astfem_sim does not only write the .auc.  Alongside it, it writes an
# experiment XML and an edit XML, and it registers a buffer, one analyte per
# species and a solution tying them together, so the run loads exactly as it
# would after us_convert and us_edit.  Two things about what it writes have to
# change before the run is usable as a test.
#
# The edit profile's data range starts at meniscus + 0.0005 cm.  That is
# inside the rounding of the radial grid, and US_AstfemMath's interpolation
# calls exit(-3) outright -- not an error return, the process dies -- when the
# simulation grid does not reach the first edited radius.  us_3dsa_cli already
# trims to its own margins; writing the same window into the edit makes a
# us_2dsa analysis of the same data use the same radii, which is the only way
# the two results are comparable.
#
# The analytes carry the vbar the data was generated from.  Handing those to a
# 2DSA analysis would give away the answer 3DSA is being asked to find, and
# would leave us_density_match nothing to recover.  So they are rewritten to a
# decoy: a plausible value an analyst would have guessed, which the generator
# picked a fixed distance from the truth.


def set_attr(text, element, attribute, value, path):
    """Replace one attribute of one element, in place, in XML source text.

    A textual substitution rather than an XML round trip: these files carry a
    DOCTYPE and a particular element order that UltraScan writes and reads,
    and rewriting them through a parser would churn all of it to change one
    number.
    """
    pattern = re.compile(
        r'(<%s\b[^>]*?\b%s=")([^"]*)(")' % (re.escape(element),
                                            re.escape(attribute))
    )
    new_text, count = pattern.subn(lambda m: m.group(1) + value + m.group(3),
                                   text)
    if count == 0:
        raise ValueError(
            f"{path}: no <{element} ... {attribute}=\"...\"> to rewrite")
    return new_text, count


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def classify_xml(path):
    """Return 'edit', 'experiment' or None for a file in a run directory."""
    try:
        head = read_text(path)[:400]
    except (OSError, UnicodeDecodeError):
        return None
    if "UltraScanEdits" in head:
        return "edit"
    if "US_Scandata" in head:
        return "experiment"
    return None


def rewrite_edit(path, meniscus, bottom, top_margin, bottom_margin):
    """Widen the edit profile's radial range away from the meniscus.

    Also moves the baseline and plateau radii inside the new range: the
    simulator places the baseline at meniscus + 0.0055 cm, which the new left
    edge would otherwise overtake.
    """
    left   = meniscus + top_margin
    right  = bottom - bottom_margin

    text = read_text(path)
    text, _ = set_attr(text, "data_range", "left",  "%.8f" % left,  path)
    text, _ = set_attr(text, "data_range", "right", "%.8f" % right, path)
    # A baseline just inside the left edge, and a plateau just inside the
    # right one, both within the range the fit will see.
    text, _ = set_attr(text, "baseline", "radius",
                       "%.8f" % (left + 0.005), path)
    text, _ = set_attr(text, "plateau", "radius",
                       "%.8f" % (right - 0.010), path)
    write_text(path, text)
    return left, right


def find_by_guid(directory, prefix, element, guid):
    """The file in *directory* whose *element* carries this GUID.

    Matched on the quoted GUID rather than on an attribute name: a solution
    names its analytes with ``guid=`` but an analyte file spells its own
    ``analyteGUID=``, and a 36-character UUID inside quotes in a file of the
    right kind is not ambiguous.
    """
    if not os.path.isdir(directory):
        return None
    needle = '"%s"' % guid
    for name in sorted(os.listdir(directory)):
        if not (name.startswith(prefix) and name.endswith(".xml")):
            continue
        path = os.path.join(directory, name)
        try:
            text = read_text(path)
        except (OSError, UnicodeDecodeError):
            continue
        if needle in text and ("<%s" % element) in text:
            return path
    return None


def solution_guid_of(experiment_xml):
    """The solution GUID the experiment XML names for its first data set."""
    text = read_text(experiment_xml)
    match = re.search(r'<solution\b[^>]*?\bguid="([^"]*)"', text)
    return match.group(1) if match else None


def rewrite_vbar(datadir, experiment_xml, decoy_vbar):
    """Plant the decoy vbar in the solution and every analyte it names.

    Returns (solution_path, [analyte_paths]).  Both records carry vbar: the
    solution keeps a copy per analyte plus a commonVbar20, and each analyte
    file has its own.  A reader may take either, so both are rewritten.
    """
    guid = solution_guid_of(experiment_xml)
    if not guid:
        raise ValueError(f"{experiment_xml}: no solution GUID")

    solution = find_by_guid(os.path.join(datadir, "solutions"),
                            "S", "solution", guid)
    if not solution:
        raise ValueError(
            f"no solution with GUID {guid} under {datadir}/solutions")

    text = read_text(solution)
    analyte_guids = re.findall(r'<analyte\b[^>]*?\bguid="([^"]*)"', text)

    text, _ = set_attr(text, "solution", "commonVbar20",
                       "%.6f" % decoy_vbar, solution)
    # One <analyte> entry per species, all rewritten.
    text = re.sub(r'(<analyte\b[^>]*?\bvbar20=")([^"]*)(")',
                  lambda m: m.group(1) + ("%.6f" % decoy_vbar) + m.group(3),
                  text)
    write_text(solution, text)

    analytes = []
    for aguid in analyte_guids:
        apath = find_by_guid(os.path.join(datadir, "analytes"),
                             "A", "analyte", aguid)
        if not apath:
            raise ValueError(
                f"no analyte with GUID {aguid} under {datadir}/analytes")
        atext = read_text(apath)
        # The element naming the vbar depends on the analyte type: PROTEIN
        # writes it on <protein>, the nucleotide types on their own elements.
        atext, count = re.subn(r'(\bvbar20=")([^"]*)(")',
                               lambda m: m.group(1) + ("%.6f" % decoy_vbar)
                                         + m.group(3),
                               atext)
        if count == 0:
            raise ValueError(f"{apath}: no vbar20 attribute to rewrite")
        write_text(apath, atext)
        analytes.append(apath)

    return solution, analytes


def prepare_run(step, datadir):
    """Make one simulated run analysable, and hide the truth from it.

    Returns a dict describing what was changed, or raises on anything the
    simulator did not write where it was expected.
    """
    outdir = step["outdir"]

    edits = []
    experiments = []
    for name in sorted(os.listdir(outdir)):
        if not name.endswith(".xml"):
            continue
        path = os.path.join(outdir, name)
        kind = classify_xml(path)
        if kind == "edit":
            edits.append(path)
        elif kind == "experiment":
            experiments.append(path)

    if not edits:
        raise ValueError(f"{outdir}: us_astfem_sim wrote no edit XML")
    if not experiments:
        raise ValueError(f"{outdir}: us_astfem_sim wrote no experiment XML")

    left, right = rewrite_edit(
        edits[0],
        step["meniscus"], step["bottom"],
        step["edit_margin"], step["edit_bottom_margin"],
    )

    solution, analytes = rewrite_vbar(datadir, experiments[0],
                                      step["decoy_vbar"])

    return {
        "edit": edits[0],
        "experiment": experiments[0],
        "data_range": [left, right],
        "solution": solution,
        "analytes": analytes,
        "decoy_vbar": step["decoy_vbar"],
        "truth_vbar": step["truth_vbar"],
    }


def write_analysis_manifest(logdir, case, prepared, datadir):
    """Record what a 2DSA cross-check of this case would need.

    us_2dsa and us_density_match are GUI programs and the harness does not
    drive them.  What it can do is leave the run in a state where they will
    load it, and say in one place where everything is: the work tree to point
    UltraScan at, each run's edit profile and radial range, the analyte and
    solution records now carrying the decoy, and the truth those records no
    longer mention.  See the harness README.
    """
    entries = []
    for step, prep in zip(case["sim_steps"], prepared):
        entries.append({
            "runid": step["runid"],
            "run_dir": step["outdir"],
            "edit_xml": prep["edit"],
            "experiment_xml": prep["experiment"],
            "data_range": prep["data_range"],
            "solution_xml": prep["solution"],
            "analyte_xml": prep["analytes"],
        })

    manifest = {
        "case": case["name"],
        "us3_work_tree": os.path.dirname(datadir),
        "data_dir": datadir,
        "truth_vbar": prepared[0]["truth_vbar"] if prepared else None,
        "decoy_vbar": prepared[0]["decoy_vbar"] if prepared else None,
        "runs": entries,
    }

    path = os.path.join(logdir, case["name"] + ".analysis.json")
    with open(path, "w") as handle:
        json.dump(manifest, handle, indent=4)
    return path


def patch_case_auc(config_path, auc_by_index):
    """Point the case file at the .auc files the simulator actually wrote."""
    with open(config_path) as handle:
        case = json.load(handle)

    for index, path in auc_by_index.items():
        case["datasets"][index]["auc"] = path

    with open(config_path, "w") as handle:
        json.dump(case, handle, indent=4)

    return case


def fit_one(case, us_3dsa_cli, resultdir, threads, timeout):
    """Run us_3dsa_cli fit for one case."""
    os.makedirs(resultdir, exist_ok=True)
    result_json = os.path.join(resultdir, case["name"] + ".json")
    model_xml = os.path.join(resultdir, case["name"] + ".model.xml")

    cmd = [
        us_3dsa_cli, "fit",
        "--config", case["config"],
        "--out", result_json,
        "--model", model_xml,
    ]
    if threads:
        cmd += ["--threads", str(threads)]

    ok, output, secs = run(cmd, timeout, case["name"])

    parsed = None
    if os.path.isfile(result_json):
        try:
            with open(result_json) as handle:
                parsed = json.load(handle)
        except json.JSONDecodeError:
            parsed = None

    return ok, output, parsed, secs


def report(results, logdir):
    """Print the summary table and return the number of failures."""
    print()
    print("=" * 100)
    print("3DSA harness summary")
    print("=" * 100)
    print()

    header = (
        f"{'case':<44} {'sets':>4} {'global RMSD':>13} "
        f"{'vbar fit':>9} {'vbar true':>9} {'err':>8} {'':>6}"
    )
    print(header)
    print("-" * len(header))

    failures = 0

    for entry in results:
        name = entry["name"]
        parsed = entry["parsed"]

        if parsed is None:
            print(f"{name:<44} {'':>4} {'no result':>13}   ** ERROR **")
            failures += 1
            continue

        if not parsed.get("accepted", False):
            verdict = "PASS" if parsed.get("pass") else "FAIL"
            print(f"{name:<44} {'':>4} {'refused':>13} "
                  f"{'':>9} {'':>9} {'':>8} {verdict:>6}")
            if verdict == "FAIL":
                failures += 1
            continue

        nsets = sum(1 for k in parsed if k.startswith("dataset_"))
        verdict = "PASS" if parsed.get("pass") else "FAIL"
        if verdict == "FAIL":
            failures += 1

        print(
            f"{name:<44} {nsets:>4} {parsed['rmsd_global']:>13.4e} "
            f"{parsed['vbar_fit']:>9.4f} {parsed['vbar_true']:>9.4f} "
            f"{parsed['vbar_fit'] - parsed['vbar_true']:>8.4f} {verdict:>6}"
        )

        # Per-data-set RMSD, which is the point of the exercise.
        for index in range(nsets):
            ds = parsed[f"dataset_{index}"]
            line = (
                f"     {ds['label']:<39} "
                f"rho {ds['density']:.4f}  "
                f"RMSD {ds['rmsd']:.4e}  "
                f"scale {ds['scale_fit']:.4f} (true {ds['scale_true']:.4f})"
            )
            # Where a series carries different noise cell by cell, the
            # aggregate RMSD says nothing; each cell's own level and cap do.
            cap = ds.get("rmsd_max", 0.0)
            if cap:
                verdict = "ok" if ds.get("rmsd_ok", True) else "OVER"
                line += (f"  noise {ds.get('rnoise', 0.0):.4f}"
                         f"  cap {cap:.4f} {verdict}")
            print(line)

    print()
    print(f"{len(results)} cases, {failures} failed")
    print(f"logs and per-case results in {logdir}")
    return failures


def main():
    parser = argparse.ArgumentParser(
        description="Run the 3DSA simulation and fit harness.")
    parser.add_argument("--outdir", required=True,
                        help="working directory for cases, data and results")
    parser.add_argument("--bindir",
                        help="directory holding us_3dsa_cli and us_astfem_sim")
    parser.add_argument("--us-3dsa-cli", dest="us_3dsa_cli",
                        help="explicit path to us_3dsa_cli")
    parser.add_argument("--us-astfem-sim", dest="us_astfem_sim",
                        help="explicit path to us_astfem_sim")
    parser.add_argument("--only", default="*",
                        help="glob selecting which cases to run")
    parser.add_argument("--jobs", type=int, default=1,
                        help="cases to fit concurrently")
    parser.add_argument("--threads", type=int, default=0,
                        help="worker threads per fit (0 = the case default)")
    parser.add_argument("--skip-gen", action="store_true",
                        help="reuse an existing case tree")
    parser.add_argument("--skip-sim", action="store_true",
                        help="reuse existing .auc data")
    parser.add_argument("--us3home",
                        help="UltraScan work tree for the run "
                             "(default <outdir>/us3home)")
    parser.add_argument("--sim-timeout", type=int, default=1800)
    parser.add_argument("--fit-timeout", type=int, default=7200)
    args = parser.parse_args()

    us_3dsa_cli = find_binary(args.us_3dsa_cli, args.bindir, "us_3dsa_cli")
    outdir = os.path.abspath(args.outdir)
    logdir = os.path.join(outdir, "results")
    os.makedirs(logdir, exist_ok=True)

    # Give the run its own UltraScan installation.  us_astfem_sim registers
    # analytes, buffers and solutions under $HOME/ultrascan/data; without this
    # a harness run would write into the user's real store, and successive
    # runs would accumulate records there.
    us3home = args.us3home or os.path.join(outdir, "us3home")
    us3home = os.path.abspath(us3home)
    datadir = os.path.join(us3home, "ultrascan", "data")
    for sub in ("analytes", "buffers", "solutions"):
        os.makedirs(os.path.join(datadir, sub), exist_ok=True)
    HEADLESS_ENV["HOME"] = us3home
    print(f"UltraScan work tree: {us3home}")

    # ---- stage 1: the case tree ------------------------------------------
    manifest_path = os.path.join(outdir, "manifest.json")

    if not args.skip_gen:
        print(f"generating cases in {outdir}")
        ok, output, _ = run(
            [us_3dsa_cli, "gen-cases", "--outdir", outdir], 600, "gen-cases")
        if not ok:
            sys.exit(f"gen-cases failed:\n{output}")
        print(output.strip())

    if not os.path.isfile(manifest_path):
        sys.exit(f"no manifest at {manifest_path}; drop --skip-gen")

    with open(manifest_path) as handle:
        manifest = json.load(handle)

    cases = [c for c in manifest["cases"] if fnmatch.fnmatch(c["name"], args.only)]
    if not cases:
        sys.exit(f"no case matches {args.only!r}")

    print(f"{len(cases)} cases selected")

    # ---- stage 2: simulate -----------------------------------------------
    if not args.skip_sim:
        us_astfem_sim = find_binary(
            args.us_astfem_sim, args.bindir, "us_astfem_sim")
        print(f"simulating with {us_astfem_sim}")

        for case in cases:
            auc_by_index = {}
            prepared = []
            for index, step in enumerate(case["sim_steps"]):
                auc, output, secs = simulate_one(
                    step, us_astfem_sim, args.sim_timeout)
                if auc is None:
                    print(f"  {case['name']} {step['runid']}: FAILED")
                    print(output)
                    break

                # Stage 3: make the run analysable and take the truth out of
                # the records it registered.
                try:
                    prep = prepare_run(step, datadir)
                except (OSError, ValueError) as exc:
                    print(f"  {case['name']} {step['runid']}: "
                          f"PREPARE FAILED: {exc}")
                    break

                prepared.append(prep)
                auc_by_index[index] = auc
                print(f"  {case['name']} {step['runid']}: "
                      f"{os.path.basename(auc)} ({secs:.1f} s)  "
                      f"range {prep['data_range'][0]:.4f}-"
                      f"{prep['data_range'][1]:.4f}  "
                      f"vbar {prep['truth_vbar']:.4f}->"
                      f"{prep['decoy_vbar']:.4f}")
            else:
                patch_case_auc(case["config"], auc_by_index)
                write_analysis_manifest(logdir, case, prepared, datadir)
                continue
            case["sim_failed"] = True
    else:
        print("reusing existing .auc data")

    runnable = [c for c in cases if not c.get("sim_failed")]

    # ---- stage 3: fit -----------------------------------------------------
    print(f"fitting {len(runnable)} cases")
    results = []

    def do_fit(case):
        ok, output, parsed, secs = fit_one(
            case, us_3dsa_cli, logdir, args.threads, args.fit_timeout)
        with open(os.path.join(logdir, case["name"] + ".log"), "w") as handle:
            handle.write(output)
        return {"name": case["name"], "ok": ok, "parsed": parsed,
                "secs": secs, "output": output}

    if args.jobs > 1:
        with futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for entry in pool.map(do_fit, runnable):
                print(f"  {entry['name']}: {entry['secs']:.1f} s")
                results.append(entry)
    else:
        for case in runnable:
            entry = do_fit(case)
            print(entry["output"])
            results.append(entry)

    for case in cases:
        if case.get("sim_failed"):
            results.append({"name": case["name"], "ok": False,
                            "parsed": None, "secs": 0.0, "output": ""})

    results.sort(key=lambda e: e["name"])
    failures = report(results, logdir)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
