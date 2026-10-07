"""Benchmark design:  factor levels, blocks and the list of tasks.

A condition is one combination of factor levels.  A task is one replicate
of one condition; it is simulated once and analysed by every arm, so the
arms are compared on identical data (paired design).
"""

import hashlib
import itertools
import json
import os

from . import systems

FACTORS = ["system", "speed", "ti_noise", "ri_noise", "random_noise",
           "local_noise", "baseline", "range_end"]

ARMS = ["main_old", "main_new", "branch_old", "branch_new"]


def _merge(base, over):
    out = dict(base)
    for key, val in over.items():
        if isinstance(val, dict) and isinstance(out.get(key), dict):
            out[key] = _merge(out[key], val)
        else:
            out[key] = val
    return out


def load(path):
    """Load a design file, resolving 'inherit' relative to its directory."""
    with open(path) as fh:
        design = json.load(fh)
    parent = design.pop("inherit", None)
    if parent:
        base = load(os.path.join(os.path.dirname(path), parent))
        base.pop("blocks", None)
        design = _merge(base, design)
    for sid in design["factors"]["system"]:
        if sid not in systems.SYSTEMS:
            raise ValueError("unknown system " + sid)
    return design


def condition_id(cond):
    """Stable short identifier of a condition."""
    text = json.dumps({k: cond[k] for k in FACTORS}, sort_keys=True)
    return hashlib.sha1(text.encode()).hexdigest()[:12]


def _seed(*parts):
    text = "|".join(str(p) for p in parts)
    return int(hashlib.sha1(text.encode()).hexdigest()[:8], 16) or 1


def conditions(design):
    """All distinct conditions of the design, each tagged with its blocks."""
    fac = design["factors"]
    ref = design["reference"]
    seen = {}

    def add(cond, block):
        cid = condition_id(cond)
        if cid not in seen:
            seen[cid] = dict(cond, condition=cid, blocks=[])
        if block not in seen[cid]["blocks"]:
            seen[cid]["blocks"].append(block)

    for blk in design["blocks"]:
        name = blk["name"]
        levels = dict(fac)
        levels.update(blk.get("levels", {}))
        fixed = dict(ref)
        fixed.update(blk.get("fixed", {}))
        crossed = blk["crossed"]

        if blk["type"] == "factorial":
            for combo in itertools.product(*[levels[f] for f in crossed]):
                cond = dict(fixed)
                cond.update(zip(crossed, combo))
                add(cond, name)

        elif blk["type"] == "one_at_a_time":
            for combo in itertools.product(*[levels[f] for f in crossed]):
                base = dict(fixed)
                base.update(zip(crossed, combo))
                add(dict(base), name)
                for factor in blk["vary"]:
                    for lev in levels[factor]:
                        cond = dict(base)
                        cond[factor] = lev
                        add(cond, name)
        else:
            raise ValueError("unknown block type " + blk["type"])

    out = list(seen.values())
    if design.get("skip_unreasonable"):
        from . import physics
        out = [c for c in out if physics.reasonable(
            systems.solutes(c["system"]), int(c["speed"]),
            design["simulation"])]
    out.sort(key=lambda c: [str(c[f]) for f in FACTORS])
    return out


def arms_for(design, task, arms=None):
    """The arms to run for a task:  design["arm_replicates"] may limit an
    arm to some replicates, e.g. {"branch_old": [0]}."""
    lim = design.get("arm_replicates", {})
    return [a for a in (arms or ARMS)
            if a not in lim or task["replicate"] in lim[a]]


def tasks(design):
    """All tasks (condition x replicate) with their seeds.

    noise_seed drives the simulated noise of the task.  geometry_seed drives
    the meniscus/bottom error of the edit; it depends only on system, speed
    and replicate, so a sweep over a noise factor sees the same edit errors
    (common random numbers).
    """
    base = design.get("base_seed", 1)
    out = []
    for cond in conditions(design):
        for rep in range(design["replicates"]):
            task = dict(cond)
            task["replicate"] = rep
            task["task"] = "%s_r%d" % (cond["condition"], rep)
            task["noise_seed"] = _seed(base, cond["condition"], rep)
            task["geometry_seed"] = _seed(base, cond["system"],
                                          cond["speed"], rep)
            out.append(task)
    return out
