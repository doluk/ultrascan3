"""Progress of a benchmark run from its result files (no SLURM
accounting needed):  finished and failed arm runs, recent rate, ETA."""

import glob
import json
import os
import time

from . import design as dsg


def status(outdir):
    meta = os.path.join(outdir, "design.json")
    if not os.path.exists(meta):
        print("no design.json in %s (no task has started yet)" % outdir)
        return
    with open(meta) as fh:
        design = json.load(fh)
    total = len(dsg.tasks(design)) * len(dsg.ARMS)

    files = [f for f in glob.glob(os.path.join(outdir, "*", "*.json"))
             if os.path.basename(os.path.dirname(f)) != "summary"]
    now = time.time()
    ok = failed = 0
    errors = {}
    mtimes = []
    for f in files:
        try:
            with open(f) as fh:
                rec = json.load(fh)
        except (OSError, ValueError):
            continue                     # being written
        mtimes.append(os.path.getmtime(f))
        if rec.get("status") == "ok":
            ok += 1
        else:
            failed += 1
            msg = rec.get("error", "?").split("\n")[0][:100]
            errors[msg] = errors.get(msg, 0) + 1
    done = ok + failed
    print("design      %s" % design.get("name"))
    print("arm runs    %d / %d done (%.1f %%):  %d ok, %d failed"
          % (done, total, 100.0 * done / total if total else 0, ok, failed))
    for hours in (1, 6, 24):
        n = sum(1 for t in mtimes if now - t < hours * 3600)
        print("last %2d h   %d arm runs (%.1f per hour)"
              % (hours, n, n / float(hours)))
    recent = sum(1 for t in mtimes if now - t < 6 * 3600) / 6.0
    if recent > 0 and done < total:
        eta = (total - done) / recent
        print("ETA         %.1f h (%.1f days) at the 6-hour rate"
              % (eta, eta / 24.0))
    if mtimes:
        print("last result %.0f min ago" % ((now - max(mtimes)) / 60.0))
    if errors:
        print("errors:")
        for msg, n in sorted(errors.items(), key=lambda kv: -kv[1])[:10]:
            print("  %5d  %s" % (n, msg))
