#!/usr/bin/env python3
"""Drive the 3DSA test harness end to end.

Three stages:

  1. ``us_3dsa_cli gen-cases`` writes the case tree -- model, buffer and
     simulation-parameter XML for every data set of every case, plus a
     manifest naming the simulator runs.
  2. ``us_astfem_sim`` is run once per data set, from the manifest, to produce
     the .auc data.
  3. ``us_3dsa_cli fit`` is run once per case and reports the global RMSD and
     the RMSD of every data set.

The .auc file name us_astfem_sim chooses depends on the optical type, cell,
channel and wavelength it settles on, so the driver discovers the file it
actually wrote rather than predicting it, and patches the case file before
fitting.

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
import shutil
import subprocess
import sys
import time

# us_astfem_sim links the Qt GUI stack even when driven headless, so it needs
# a platform plugin that does not want a display.
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
            print(
                f"     {ds['label']:<39} "
                f"rho {ds['density']:.4f}  "
                f"RMSD {ds['rmsd']:.4e}  "
                f"scale {ds['scale_fit']:.4f} (true {ds['scale_true']:.4f})"
            )

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
    parser.add_argument("--sim-timeout", type=int, default=1800)
    parser.add_argument("--fit-timeout", type=int, default=7200)
    args = parser.parse_args()

    us_3dsa_cli = find_binary(args.us_3dsa_cli, args.bindir, "us_3dsa_cli")
    outdir = os.path.abspath(args.outdir)
    logdir = os.path.join(outdir, "results")
    os.makedirs(logdir, exist_ok=True)

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
            for index, step in enumerate(case["sim_steps"]):
                auc, output, secs = simulate_one(
                    step, us_astfem_sim, args.sim_timeout)
                if auc is None:
                    print(f"  {case['name']} {step['runid']}: FAILED")
                    print(output)
                    break
                auc_by_index[index] = auc
                print(f"  {case['name']} {step['runid']}: "
                      f"{os.path.basename(auc)} ({secs:.1f} s)")
            else:
                patch_case_auc(case["config"], auc_by_index)
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
