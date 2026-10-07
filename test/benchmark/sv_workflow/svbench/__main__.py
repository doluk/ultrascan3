"""Command line of the SV workflow benchmark.

  python -m svbench count     DESIGN
  python -m svbench list      DESIGN [--out tasks.tsv]
  python -m svbench split     DESIGN --stride N --elements A-B --out FILE
  python -m svbench run       DESIGN CONFIG OUTDIR (--index I [--stride N]
                              | --task ID | --all) [--task-file FILE]
                              [--claim] [--max-tasks K] [--max-minutes M]
                              [--arms A,B] [--keep]
  python -m svbench status    OUTDIR [--config CONFIG] [--task-file FILE]
  python -m svbench check     CONFIG
  python -m svbench preview   DESIGN CONFIG OUTDIR [--jobs N]
  python -m svbench reset-failed OUTDIR
  python -m svbench aggregate OUTDIR [--out DIR]
  python -m svbench report    SUMMARY_DIR [--out report.html]
"""

import argparse
import os
import sys
import time

from . import design as dsg


def main(argv=None):
    ap = argparse.ArgumentParser(prog="svbench")
    sub = ap.add_subparsers(dest="cmd")
    sub.required = True             # Python 3.6 has no required=

    p = sub.add_parser("count", help="number of conditions and tasks")
    p.add_argument("design")

    p = sub.add_parser("list", help="list the tasks of a design")
    p.add_argument("design")
    p.add_argument("--out")

    p = sub.add_parser("split", help="task file for another cluster:  "
                       "the tasks of some elements of a stride array")
    p.add_argument("design")
    p.add_argument("--stride", type=int, required=True,
                   help="STRIDE (array size) of the existing stride run")
    p.add_argument("--elements", required=True,
                   help="array elements whose tasks to take, e.g. 230-999")
    p.add_argument("--exclude-done",
                   help="output directory:  leave out tasks with all "
                        "results")
    p.add_argument("--out", required=True)

    p = sub.add_parser("run", help="run tasks")
    p.add_argument("design")
    p.add_argument("config")
    p.add_argument("outdir")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--index", type=int,
                   help="task index (e.g. SLURM_ARRAY_TASK_ID)")
    g.add_argument("--task", help="task id")
    g.add_argument("--all", action="store_true")
    p.add_argument("--stride", type=int, default=0,
                   help="with --index: run tasks index, index+stride, ...")
    p.add_argument("--task-file",
                   help="run only the tasks of this file (svbench split), "
                        "in its order; --index/--stride/--all select "
                        "within it")
    p.add_argument("--claim", action="store_true",
                   help="take the next task no other job has claimed "
                        "(shared OUTDIR on one cluster)")
    p.add_argument("--reclaim-hours", type=float, default=4.0,
                   help="with --claim:  take over claims older than this "
                        "whose task has no results")
    p.add_argument("--max-tasks", type=int, default=0,
                   help="stop after this many tasks (0: no limit)")
    p.add_argument("--max-minutes", type=float, default=0,
                   help="start no new task after this many minutes")
    p.add_argument("--arms", default=",".join(dsg.ARMS))
    p.add_argument("--retry-failed", action="store_true",
                   help="run again the arms whose result is an error")
    p.add_argument("--keep", action="store_true",
                   help="keep the scratch directory of each task")
    p.add_argument("--timeout", type=float, default=None,
                   help="time limit (s) of one us_mpi_analysis job")

    p = sub.add_parser("status", help="progress of a run (result files)")
    p.add_argument("outdir")
    p.add_argument("--config",
                   help="also detect running arms from the scratch "
                        "directory (jobs started without run markers)")
    p.add_argument("--task-file",
                   help="count only the tasks of this file (this "
                        "cluster's share)")
    p.add_argument("--stale-hours", type=float, default=6.0,
                   help="age after which a running marker counts as stale")

    p = sub.add_parser("preview", help="noise-free simulation of every "
                       "system x speed, with plots and observability")
    p.add_argument("design")
    p.add_argument("config")
    p.add_argument("outdir")
    p.add_argument("--range-end", type=float, default=None,
                   help="data range end before the bottom (cm; default: "
                        "the design's reference)")
    p.add_argument("--replicate", type=int, default=0,
                   help="replicate whose edit geometry to use")
    p.add_argument("--systems", help="comma-separated subset")
    p.add_argument("--speeds", help="comma-separated subset")
    p.add_argument("--jobs", type=int, default=4,
                   help="simulations in parallel")
    p.add_argument("--keep", action="store_true",
                   help="keep the raw simulated data (AUC, time state, "
                        "edit) in OUTDIR/raw/<system>/<speed>rpm/")

    p = sub.add_parser("check", help="can the configured programs start "
                       "on this node?  (exit 1 if not)")
    p.add_argument("config")

    p = sub.add_parser("reset-failed", help="delete failed results (and "
                       "their claims) so that they are run again")
    p.add_argument("outdir")
    p.add_argument("--only-simulation", action="store_true",
                   help="only results failed in the simulation step")

    p = sub.add_parser("aggregate", help="collect results into CSV")
    p.add_argument("outdir")
    p.add_argument("--out", default=None)

    p = sub.add_parser("report", help="HTML report from aggregated CSV")
    p.add_argument("summary")
    p.add_argument("--out", default=None)

    a = ap.parse_args(argv)

    if a.cmd in ("count", "list", "run", "split"):
        design = dsg.load(a.design)
        tasks = dsg.tasks(design)

    if a.cmd == "count":
        conds = dsg.conditions(design)
        blocks = {}
        for c in conds:
            for b in c["blocks"]:
                blocks[b] = blocks.get(b, 0) + 1
        print("design      %s" % design["name"])
        for b, n in blocks.items():
            print("block       %-20s %d conditions" % (b, n))
        print("conditions  %d" % len(conds))
        print("replicates  %d" % design["replicates"])
        print("tasks       %d" % len(tasks))
        print("arm runs    %d" % (len(tasks) * len(dsg.ARMS)))

    elif a.cmd == "list":
        fh = open(a.out, "w") if a.out else sys.stdout
        cols = ["index", "task"] + dsg.FACTORS + ["replicate", "blocks"]
        fh.write("\t".join(cols) + "\n")
        for i, t in enumerate(tasks):
            row = [str(i), t["task"]] + [str(t[f]) for f in dsg.FACTORS] \
                + [str(t["replicate"]), ",".join(t["blocks"])]
            fh.write("\t".join(row) + "\n")

    elif a.cmd == "split":
        from . import claim, runner
        sel = claim.split(tasks, a.stride, claim.parse_elements(a.elements))
        if a.exclude_done:
            sel = [t for t in sel if not all(
                os.path.exists(runner.result_path(a.exclude_done, t, arm))
                for arm in dsg.ARMS)]
        with open(a.out, "w") as fh:
            fh.write("# %s: elements %s of stride %d, %d tasks\n"
                     % (design["name"], a.elements, a.stride, len(sel)))
            for t in sel:
                fh.write(t["task"] + "\n")
        print("%d tasks -> %s" % (len(sel), a.out))

    elif a.cmd == "run":
        from . import claim, runner
        cfg = runner.load_config(a.config)
        if a.task_file:
            tasks = claim.read_task_file(a.task_file, tasks)
        if a.all:
            sel = tasks
        elif a.task:
            sel = [t for t in tasks if t["task"] == a.task]
        elif a.stride > 0:
            sel = tasks[a.index::a.stride]
        else:
            sel = [tasks[a.index]] if a.index < len(tasks) else []
        if not sel:
            print("no task selected", file=sys.stderr)
            return 0 if a.task_file else 1
        arms = [x for x in a.arms.split(",") if x]
        outdir = os.path.abspath(a.outdir)
        runner.check_outdir(design, outdir)
        t0 = time.time()
        bad = ran = sim_fail = 0
        limited = False                  # stopped by a limit, work left
        for t in sel:
            if os.path.exists(os.path.join(outdir, ".stop")):
                limited = False
                break
            if (a.max_tasks and ran >= a.max_tasks) or \
                    (a.max_minutes and time.time() - t0 > a.max_minutes * 60):
                limited = True
                break

            def is_done(t=t):
                return all(runner._done(runner.result_path(outdir, t, arm),
                                        a.retry_failed) for arm in arms)
            if is_done():
                continue
            if a.claim and not claim.claim(outdir, t["task"],
                                           a.reclaim_hours, is_done):
                continue
            ran += 1
            res = runner.run_task(design, t, cfg, outdir, arms, a.keep,
                                  a.timeout, a.retry_failed)
            for arm, status in res:
                print("%s %-11s %s" % (t["task"], arm, status), flush=True)
                bad += status != "ok"
            # Simulations failing task after task:  this node cannot run
            # the simulator; stop instead of failing every task
            if res and all(st != "ok" for _, st in res) and \
                    runner.simulation_failed(outdir, t, res[0][0]):
                sim_fail += 1
                if sim_fail >= 2:
                    print("simulation failed for %d tasks in a row on %s; "
                          "stopping" % (sim_fail, os.uname()[1]),
                          file=sys.stderr)
                    return 4
            else:
                sim_fail = 0
        if limited:
            return 3                     # more tasks may be left
        return 1 if bad else 0

    elif a.cmd == "status":
        from . import status
        status.status(a.outdir, a.config, a.stale_hours, a.task_file)

    elif a.cmd == "preview":
        from . import preview, runner
        design = dsg.load(a.design)
        return preview.preview(
            design, runner.load_config(a.config), a.outdir, a.range_end,
            a.replicate,
            a.systems.split(",") if a.systems else None,
            [int(x) for x in a.speeds.split(",")] if a.speeds else None,
            a.jobs, a.keep)

    elif a.cmd == "check":
        from . import check, runner
        return check.main(runner.load_config(a.config))

    elif a.cmd == "reset-failed":
        from . import runner
        return runner.reset_failed(a.outdir, a.only_simulation)

    elif a.cmd == "aggregate":
        from . import aggregate
        aggregate.aggregate(a.outdir, a.out or a.outdir + "/summary")

    elif a.cmd == "report":
        from . import report
        report.report(a.summary, a.out or a.summary + "/report.html")
    return 0


if __name__ == "__main__":
    sys.exit(main())
