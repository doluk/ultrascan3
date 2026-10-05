"""Progress of a benchmark run from its files (no SLURM accounting
needed).

  completed  OUT/<task>/<arm>.json with status ok
  failed     OUT/<task>/<arm>.json with status error
  running    OUT/<task>/<arm>.running marker (written by run while the arm
             runs), younger than --stale-hours; with --config also arms
             whose scratch work folder exists, have no result and whose
             task log changed in the last 30 min (jobs started before
             markers existed)
  stale      markers older than --stale-hours without result (element
             killed, e.g. at its time limit); counted as pending
  pending    everything else
"""

import glob
import json
import os
import time

from . import design as dsg

ACTIVE_LOG_SECONDS = 1800


def _load(path):
    try:
        with open(path) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def _scratch_running(cfg_path, results):
    """(task, arm) pairs running according to the scratch directory."""
    from .runner import load_config
    cfg = load_config(cfg_path)
    now = time.time()
    out = set()
    for tdir in glob.glob(os.path.join(cfg["scratch"], "*")):
        log = os.path.join(tdir, "log.txt")
        if not os.path.exists(log) or \
                now - os.path.getmtime(log) > ACTIVE_LOG_SECONDS:
            continue
        task = os.path.basename(tdir)
        for arm in dsg.ARMS:
            if os.path.isdir(os.path.join(tdir, arm)) and \
                    (task, arm) not in results:
                out.add((task, arm))
    # Only the newest arm folder of a task is the running one
    newest = {}
    for task, arm in out:
        mt = os.path.getmtime(os.path.join(cfg["scratch"], task, arm))
        if task not in newest or mt > newest[task][1]:
            newest[task] = (arm, mt)
    return set((t, a) for t, (a, _) in newest.items())


def status(outdir, config=None, stale_hours=6.0):
    meta = os.path.join(outdir, "design.json")
    design = _load(meta)
    if design is None:
        print("no design.json in %s (no task has started yet)" % outdir)
        return
    total = len(dsg.tasks(design)) * len(dsg.ARMS)
    now = time.time()

    results = {}
    mtimes = []
    errors = {}
    for f in glob.glob(os.path.join(outdir, "*", "*.json")):
        task = os.path.basename(os.path.dirname(f))
        arm = os.path.basename(f)[:-5]
        if arm not in dsg.ARMS:
            continue
        rec = _load(f)
        if rec is None:
            continue
        results[(task, arm)] = rec.get("status")
        mtimes.append(os.path.getmtime(f))
        if rec.get("status") != "ok":
            msg = rec.get("error", "?").split("\n")[0][:100]
            errors[msg] = errors.get(msg, 0) + 1

    running, stale = {}, 0
    for f in glob.glob(os.path.join(outdir, "*", "*.running")):
        task = os.path.basename(os.path.dirname(f))
        arm = os.path.basename(f)[:-8]
        if (task, arm) in results:
            continue
        info = _load(f) or {}
        age = now - info.get("start", os.path.getmtime(f))
        if age > stale_hours * 3600:
            stale += 1
        else:
            running[(task, arm)] = info
    if config:
        for key in _scratch_running(config, results):
            running.setdefault(key, {"host": "?", "start": None})

    completed = sum(1 for s in results.values() if s == "ok")
    failed = len(results) - completed
    pending = total - completed - failed - len(running)

    def pct(n):
        return 100.0 * n / total if total else 0.0

    print("design     %s   (%s)" % (design.get("name"), outdir))
    print("total      %7d" % total)
    print("completed  %7d  %5.1f %%" % (completed, pct(completed)))
    print("failed     %7d  %5.1f %%" % (failed, pct(failed)))
    print("running    %7d" % len(running))
    print("pending    %7d  %5.1f %%" % (pending, pct(pending))
          + ("   (incl. %d stale markers)" % stale if stale else ""))

    for hours in (1, 6, 24):
        n = sum(1 for t in mtimes if now - t < hours * 3600)
        print("last %2d h  %7d arm runs  (%.1f / h)"
              % (hours, n, n / float(hours)))
    rate = sum(1 for t in mtimes if now - t < 6 * 3600) / 6.0
    if rate > 0 and pending + len(running) > 0:
        eta = (pending + len(running)) / rate
        print("ETA        %.1f h (%.1f days) at the 6-hour rate"
              % (eta, eta / 24.0))
    if mtimes:
        print("last result %.0f min ago" % ((now - max(mtimes)) / 60.0))

    if running:
        print("running arms:")
        for (task, arm), info in sorted(running.items()):
            start = info.get("start")
            age = "%5.0f min" % ((now - start) / 60.0) if start else "    ? min"
            where = info.get("host", "?")
            if info.get("array_task"):
                where += "  element %s" % info["array_task"]
            print("  %-18s %-11s %s  %s" % (task, arm, age, where))
    if errors:
        print("errors:")
        for msg, n in sorted(errors.items(), key=lambda kv: -kv[1])[:10]:
            print("  %5d  %s" % (n, msg))
