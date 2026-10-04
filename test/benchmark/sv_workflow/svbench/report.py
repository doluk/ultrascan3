"""Self-contained HTML report from the aggregated CSV tables.

Figures:  build (main / branch) is encoded by hue, workflow (old / new)
by line style, so every arm is identifiable without colour as well.
Every figure is followed by its numbers in a table.
"""

import base64
import csv
import html
import io
import json
import math
import os

import numpy as np

from .design import ARMS

ARM_STYLE = {
    "main_old": ("#2a78d6", "-", "o"),
    "main_new": ("#2a78d6", "--", "s"),
    "branch_old": ("#eb6834", "-", "o"),
    "branch_new": ("#eb6834", "--", "s"),
}
ARM_LABEL = {
    "main_old": "main NNLS, old workflow",
    "main_new": "main NNLS, new workflow",
    "branch_old": "branch NNLS, old workflow",
    "branch_new": "branch NNLS, new workflow",
}

# metric, label, transform ("abs" = absolute value), lower-is-better
METRICS = [
    ("rmsd_ratio", "RMSD / true random-noise RMS", None),
    ("noise_rmsd", "TI+RI noise error, RMS (OD)", None),
    ("ti_shape_rmsd", "TI noise shape error, RMS (OD)", None),
    ("ri_shape_rmsd", "RI noise shape error, RMS (OD)", None),
    ("total_conc_relerr", "|total concentration error| (rel.)", "abs"),
    ("conc_relerr_max", "max |species concentration error| (rel.)", None),
    ("s_relerr_max", "max |species s error| (rel.)", None),
    ("ff0_relerr_max", "max |species f/f0 error| (rel.)", None),
    ("ghost_conc_fraction", "ghost concentration fraction", None),
    ("meniscus_fit_err", "|meniscus error| after fit (cm)", "abs"),
    ("bottom_fit_err", "|bottom error| after fit (cm)", "abs"),
    ("wall_seconds", "wall time per arm (s)", None),
]
SWEEP_METRICS = ["rmsd_ratio", "noise_rmsd", "total_conc_relerr",
                 "conc_relerr_max", "s_relerr_max", "meniscus_fit_err",
                 "bottom_fit_err"]
OAT_FACTORS = ["ti_noise", "ri_noise", "random_noise", "local_noise",
               "baseline", "range_end", "speed"]
FACTOR_LABEL = {
    "ti_noise": "TI noise (% of total conc. per radial step, random walk)",
    "ri_noise": "RI noise (% of total conc. per scan)",
    "random_noise": "random noise (% of total conc.)",
    "local_noise": "random noise (% of local conc.)",
    "baseline": "baseline offset (OD)",
    "range_end": "data range end before bottom (cm)",
    "speed": "rotor speed (rpm)",
}


def _f(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return float("nan")


def _val(row, metric):
    v = _f(row.get(metric))
    tr = dict((m, t) for m, _, t in METRICS).get(metric)
    return abs(v) if tr == "abs" else v


def _read(path):
    if not os.path.exists(path):
        return []
    with open(path) as fh:
        return list(csv.DictReader(fh))


def _median(vals):
    vals = [v for v in vals if not math.isnan(v)]
    return float(np.median(vals)) if vals else float("nan")


def _fmt(v):
    if isinstance(v, str):
        return html.escape(v)
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "–"
    if v == 0:
        return "0"
    a = abs(v)
    if a >= 1000 or a < 1e-3:
        return "%.2e" % v
    return "%.4g" % v


def _table(head, rows, cls=""):
    out = ['<div class="tw"><table class="%s"><thead><tr>' % cls]
    out += ["<th>%s</th>" % html.escape(str(h)) for h in head]
    out.append("</tr></thead><tbody>")
    for r in rows:
        out.append("<tr>" + "".join("<td>%s</td>" % _fmt(c) for c in r)
                   + "</tr>")
    out.append("</tbody></table></div>")
    return "".join(out)


def _svg(fig):
    buf = io.StringIO()
    fig.savefig(buf, format="svg", bbox_inches="tight")
    import matplotlib.pyplot as plt
    plt.close(fig)
    svg = buf.getvalue()
    return '<div class="fig">%s</div>' % svg[svg.index("<svg"):]


def _ref_filter(runs, reference, factor):
    """OAT runs:  all non-crossed factors at the reference except factor."""
    out = []
    for r in runs:
        if "oat" not in r["blocks"].split(";"):
            continue
        good = True
        for f, v in reference.items():
            if f != factor and _f(r[f]) != float(v):
                good = False
                break
        if good:
            out.append(r)
    return out


def sweep_section(runs, reference, factor):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # speed is crossed in the OAT block:  use its reference-noise runs
    sel = _ref_filter(runs, reference,
                      None if factor == "speed" else factor)
    if not sel:
        return ""
    levels = sorted(set(_f(r[factor]) for r in sel))
    if len(levels) < 2:
        return ""
    arms = [a for a in ARMS if any(r["arm"] == a for r in sel)]
    n = len(SWEEP_METRICS)
    cols = 4
    nrow = int(math.ceil(n / cols))
    fig, axes = plt.subplots(nrow, cols, figsize=(3.3 * cols, 2.6 * nrow),
                             squeeze=False)
    table = []
    for k, metric in enumerate(SWEEP_METRICS):
        ax = axes[k // cols][k % cols]
        for arm in arms:
            ys = [_median([_val(r, metric) for r in sel
                           if r["arm"] == arm and _f(r[factor]) == lev])
                  for lev in levels]
            col, ls, mk = ARM_STYLE[arm]
            xs = list(range(len(levels)))
            ax.plot(xs, ys, color=col, ls=ls, marker=mk, ms=4, lw=1.6,
                    label=ARM_LABEL[arm])
            for lev, y in zip(levels, ys):
                table.append([metric, lev, ARM_LABEL[arm], y])
        ax.set_xticks(range(len(levels)))
        ax.set_xticklabels([("%g" % v) for v in levels], fontsize=7,
                           rotation=45 if factor == "speed" else 0)
        ax.set_title(dict((m, l) for m, l, _ in METRICS)[metric], fontsize=8)
        ax.tick_params(labelsize=7)
        ax.grid(alpha=0.25, lw=0.5)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        if metric != "rmsd_ratio":
            vals = [l.get_ydata() for l in ax.get_lines()]
            flat = [v for arr in vals for v in arr if v > 0]
            if flat and max(flat) / max(min(flat), 1e-12) > 50:
                ax.set_yscale("log")
    for k in range(n, nrow * cols):
        axes[k // cols][k % cols].axis("off")
    h, l = axes[0][0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=4, fontsize=8, frameon=False,
               bbox_to_anchor=(0.5, -0.04))
    fig.suptitle(FACTOR_LABEL[factor] + "  (median over systems, speeds,"
                 " replicates; other factors at reference)", fontsize=9)
    fig.tight_layout(rect=(0, 0.03, 1, 0.95))
    ntask = len(set(r["task"] for r in sel))
    out = ['<h3>%s</h3>' % html.escape(FACTOR_LABEL[factor]),
           '<p class="note">%d tasks.</p>' % ntask, _svg(fig),
           "<details><summary>Table</summary>",
           _table(["metric", factor, "arm", "median"], table), "</details>"]
    return "".join(out)


def group_table(runs, key, metrics):
    groups = sorted(set(r[key] for r in runs),
                    key=lambda v: (_f(v) if not math.isnan(_f(v)) else 0, v))
    arms = [a for a in ARMS if any(r["arm"] == a for r in runs)]
    head = [key, "arm", "n"] + [dict((m, l) for m, l, _ in METRICS)[m]
                                for m in metrics]
    rows = []
    for g in groups:
        for arm in arms:
            sel = [r for r in runs if r[key] == g and r["arm"] == arm]
            rows.append([g, arm, len(sel)]
                        + [_median([_val(r, m) for r in sel])
                           for m in metrics])
    return _table(head, rows, "num")


def headline(runs):
    by_task = {}
    for r in runs:
        by_task.setdefault(r["task"], {})[r["arm"]] = r
    arms = [a for a in ARMS if any(r["arm"] == a for r in runs)]
    head = ["metric"] + ["%s median" % a for a in arms] \
        + ["%s better than main_old" % a for a in arms if a != "main_old"]
    rows = []
    for m, label, _ in METRICS:
        row = [label]
        for a in arms:
            row.append(_median([_val(r, m) for r in runs if r["arm"] == a]))
        for a in arms:
            if a == "main_old":
                continue
            if m == "rmsd_ratio":
                score = lambda r: abs(_val(r, m) - 1.0)
            else:
                score = lambda r: _val(r, m)
            wins = n = 0
            for arms_t in by_task.values():
                if a in arms_t and "main_old" in arms_t:
                    x, y = score(arms_t[a]), score(arms_t["main_old"])
                    if math.isnan(x) or math.isnan(y):
                        continue
                    n += 1
                    wins += (x < y) + 0.5 * (x == y)
            row.append("%.0f%% (n=%d)" % (100.0 * wins / n, n) if n else "–")
        rows.append(row)
    return _table(head, rows, "num")


def equivalence(runs):
    """Paired |old - new| per build:  does the workflow matter?"""
    by_task = {}
    for r in runs:
        by_task.setdefault(r["task"], {})[r["arm"]] = r
    head = ["metric"]
    pairs = [("main_old", "main_new"), ("branch_old", "branch_new"),
             ("main_old", "branch_old"), ("main_new", "branch_new")]
    for a, b in pairs:
        head += ["median |%s − %s|" % (a, b), "median %s − %s" % (b, a)]
    rows = []
    for m, label, _ in METRICS:
        row = [label]
        for a, b in pairs:
            d = [_val(t[b], m) - _val(t[a], m) for t in by_task.values()
                 if a in t and b in t]
            row += [_median([abs(x) for x in d]), _median(d)]
        rows.append(row)
    return _table(head, rows, "num")


CSS = """
:root { --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --rule:#e4e3df;
        --accent:#2a78d6; }
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { --surface:#1a1a19; --ink:#ffffff;
        --ink2:#c3c2b7; --rule:#3a3a37; --accent:#3987e5; } }
:root[data-theme="dark"] { --surface:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7;
        --rule:#3a3a37; --accent:#3987e5; }
body { background:var(--surface); color:var(--ink); margin:0;
       font:14px/1.5 system-ui, sans-serif; }
main { max-width:1200px; margin:0 auto; padding:16px; }
h1 { font-size:22px; } h2 { font-size:18px; margin-top:32px;
     border-bottom:1px solid var(--rule); } h3 { font-size:15px; }
.note { color:var(--ink2); }
.tw { overflow-x:auto; }
table { border-collapse:collapse; font-size:12px; margin:8px 0; }
th, td { border-bottom:1px solid var(--rule); padding:3px 8px;
         text-align:left; }
table.num td { font-variant-numeric:tabular-nums; }
.fig { background:#ffffff; border-radius:6px; padding:4px; overflow-x:auto; }
.fig svg { max-width:100%; height:auto; }
details { margin:6px 0 18px; } summary { cursor:pointer; color:var(--accent); }
code { font-size:12px; }
"""


def report(summary_dir, out_path):
    runs = _read(os.path.join(summary_dir, "runs.csv"))
    errors = _read(os.path.join(summary_dir, "errors.csv"))
    ref = {}
    meta = os.path.join(summary_dir, "design.json")
    if os.path.exists(meta):
        with open(meta) as fh:
            ref = json.load(fh).get("reference", {})

    arms = [a for a in ARMS if any(r["arm"] == a for r in runs)]
    ntask = len(set(r["task"] for r in runs))
    ncond = len(set(r["condition"] for r in runs))
    parts = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
             '<meta name="viewport" content="width=device-width,'
             'initial-scale=1">', "<title>SV workflow benchmark</title>",
             "<style>%s</style></head><body><main>" % CSS,
             "<h1>SV workflow benchmark: NNLS main vs branch, old vs new"
             " workflow</h1>",
             '<p class="note">%d tasks (%d conditions), %d successful arm'
             " runs, %d failed runs. Arms: %s.</p>"
             % (ntask, ncond, len(runs), len(errors), ", ".join(
                 ARM_LABEL[a] for a in arms)),
             "<p>Old workflow: 2DSA with TI noise &rarr; meniscus+bottom fit"
             " (TI+RI, TI noise of step 1 loaded) &rarr; fit-meniscus edit"
             " update &rarr; 2DSA iterative refinement (TI+RI). New workflow:"
             " the same without the first step. All arms analyse identical"
             " simulated data (paired design).</p>",
             "<h2>Headline</h2>",
             '<p class="note">Medians over all successful runs. "Better"'
             " is the share of tasks where the arm beats main/old on the"
             " same data (for the RMSD ratio: closer to 1).</p>",
             headline(runs),
             "<h2>Does the workflow or the NNLS change matter?</h2>",
             '<p class="note">Paired differences on identical data. With'
             " exact TI/RI elimination a pre-subtracted TI noise lies in the"
             " eliminated subspace, so the branch old and new workflows are"
             " expected to agree up to numerical noise.</p>",
             equivalence(runs)]

    if ref:
        parts.append("<h2>One-factor-at-a-time sweeps</h2>")
        parts.append('<p class="note">Reference: %s.</p>' % html.escape(
            ", ".join("%s=%s" % kv for kv in ref.items())))
        for f in OAT_FACTORS:
            parts.append(sweep_section(runs, ref, f))

    show = ["rmsd_ratio", "noise_rmsd", "total_conc_relerr",
            "conc_relerr_max", "s_relerr_max", "ff0_relerr_max",
            "meniscus_fit_err", "bottom_fit_err"]
    parts += ["<h2>By system</h2>",
              '<p class="note">Medians over all runs of each system.</p>',
              group_table(runs, "system", show),
              "<h2>By speed</h2>", group_table(runs, "speed", show)]
    if errors:
        parts += ["<h2>Failed runs</h2>",
                  _table(["task", "arm", "error"],
                         [[e["task"], e["arm"], e["error"][:300]]
                          for e in errors])]
    parts.append("</main></body></html>")
    with open(out_path, "w") as fh:
        fh.write("\n".join(p for p in parts if p))
    print("report -> " + out_path)
