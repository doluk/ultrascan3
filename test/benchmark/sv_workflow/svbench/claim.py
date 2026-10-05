"""Task lists and claims for running one design on several clusters.

Between clusters the work is split statically (`svbench split`):  each
cluster gets a task file and its own output directory;  the result
directories are disjoint and are merged afterwards (rsync).

Within a cluster, jobs started with `run --claim` take the next task of
the list that no other job has claimed.  A claim is the directory
OUT/.claims/<task>, created atomically with mkdir on the cluster's shared
file system.  A claim older than `reclaim_hours` whose task still lacks
results is taken over (its job was killed).
"""

import json
import os
import shutil
import socket
import time


def parse_elements(text):
    """'230-999' or '0-99,500-599' -> set of ints."""
    out = set()
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            out.update(range(int(lo), int(hi) + 1))
        else:
            out.add(int(part))
    return out


def split(tasks, stride, elements):
    """Tasks that the stride array elements `elements` would run, in the
    order in which a stride array progresses (round by round), so that a
    partially done list covers all systems/speeds/noise levels evenly."""
    idx = [i for i in range(len(tasks)) if i % stride in elements]
    idx.sort(key=lambda i: (i // stride, i % stride))
    return [tasks[i] for i in idx]


def read_task_file(path, tasks):
    """Tasks named in a task file (first column; '#' comments and the
    header of `svbench list` are skipped), in file order."""
    by_id = dict((t["task"], t) for t in tasks)
    out = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            tid = line.split()[0]
            if tid in ("index", "task"):
                continue
            if tid.isdigit() and len(line.split()) > 1:
                tid = line.split()[1]          # `svbench list` rows
            if tid not in by_id:
                raise SystemExit("task %s of %s is not in the design"
                                 % (tid, path))
            out.append(by_id[tid])
    return out


def claim(outdir, task_id, reclaim_hours, is_done):
    """Try to claim a task; True if this job now owns it."""
    cdir = os.path.join(outdir, ".claims")
    os.makedirs(cdir, exist_ok=True)
    path = os.path.join(cdir, task_id)
    info = {"host": socket.gethostname(), "pid": os.getpid(),
            "time": time.time(), "job": os.environ.get("SLURM_JOB_ID"),
            "array_task": os.environ.get("SLURM_ARRAY_TASK_ID")}
    for _ in range(2):
        try:
            os.mkdir(path)
        except FileExistsError:
            age = time.time() - os.path.getmtime(path)
            if age > reclaim_hours * 3600 and not is_done():
                shutil.rmtree(path, ignore_errors=True)   # dead job
                continue
            return False
        with open(os.path.join(path, "owner.json"), "w") as fh:
            json.dump(info, fh)
        return True
    return False
