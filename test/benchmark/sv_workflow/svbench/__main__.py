"""Command line of the SV workflow benchmark.

  python -m svbench count     DESIGN
  python -m svbench list      DESIGN [--out tasks.tsv]
  python -m svbench run       DESIGN CONFIG OUTDIR (--index I [--stride N]
                              | --task ID | --all) [--arms A,B] [--keep]
  python -m svbench status    OUTDIR [--config CONFIG]
  python -m svbench aggregate OUTDIR [--out DIR]
  python -m svbench report    SUMMARY_DIR [--out report.html]
"""

import argparse
import sys

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
    p.add_argument("--stale-hours", type=float, default=6.0,
                   help="age after which a running marker counts as stale")

    p = sub.add_parser("aggregate", help="collect results into CSV")
    p.add_argument("outdir")
    p.add_argument("--out", default=None)

    p = sub.add_parser("report", help="HTML report from aggregated CSV")
    p.add_argument("summary")
    p.add_argument("--out", default=None)

    a = ap.parse_args(argv)

    if a.cmd in ("count", "list", "run"):
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

    elif a.cmd == "run":
        from . import runner
        cfg = runner.load_config(a.config)
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
            return 1
        arms = [x for x in a.arms.split(",") if x]
        bad = 0
        for t in sel:
            res = runner.run_task(design, t, cfg, a.outdir, arms, a.keep,
                                  a.timeout, a.retry_failed)
            for arm, status in res:
                print("%s %-11s %s" % (t["task"], arm, status), flush=True)
                bad += status != "ok"
        return 1 if bad else 0

    elif a.cmd == "status":
        from . import status
        status.status(a.outdir, a.config, a.stale_hours)

    elif a.cmd == "aggregate":
        from . import aggregate
        aggregate.aggregate(a.outdir, a.out or a.outdir + "/summary")

    elif a.cmd == "report":
        from . import report
        report.report(a.summary, a.out or a.summary + "/report.html")
    return 0


if __name__ == "__main__":
    sys.exit(main())
