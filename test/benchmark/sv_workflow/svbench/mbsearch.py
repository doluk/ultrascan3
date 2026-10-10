"""Adaptive meniscus/bottom search, replayed on stored full grids.

Every result file holds the full meniscus x bottom grid of step 2
("fit_mb_grid", meniscus-major).  A search strategy asks for grid points
one at a time; the replay answers with the stored RMSD and counts the
points asked for.  Nothing is recomputed, so all strategies see exactly
the RMSD surface the full grid saw.

Strategies (all start at the grid centre, i.e. the edit's meniscus and
bottom, and all stop at the latest when the grid is exhausted):

  full          every grid point (the current us_mpi_analysis behaviour)
  lines         alternating line scans:  meniscus line through the
                centre bottom, then the bottom line through the best
                point, then the meniscus line through the best point, ...
                If the line through the best point was scanned already,
                the line through the next best point is taken.  Stops when
                the best point and its (in-bounds) 8 neighbours are known.
  lines_fill    alternating line scans until the best point lies on a
                scanned meniscus and a scanned bottom line, then descent
  line_descent  one meniscus line through the centre bottom, then descent
  descent       descent from the centre

Descent (on the grid):  evaluate the 3x3 around the current point; move to
the best of them if it is strictly better than the current point; repeat.
It stops at a point that is the minimum of its complete 3x3, which is what
the fit-meniscus estimator needs.

Estimators of the fitted meniscus and bottom, from the evaluated points
and the best point b (all use the in-bounds 3x3 around b):

  legacy        weights 1/rmsd - 1/rmsd_max, rmsd_max over the evaluated
                points (us_fit_meniscus for the full grid)
  local         as legacy, rmsd_max over the 3x3
  none          weights 1/rmsd
  quad          vertex of a quadratic in (meniscus, bottom) fitted to the
                RMSD of the 3x3 (least squares, interior points only);
                per-axis parabola through 3 points otherwise; each
                coordinate clamped to the 3x3
  best          the best grid point

Output (DEST):
  mbsearch_runs.csv     task x arm x strategy:  evaluations, whether the
                        global grid minimum was found, estimator errors
  mbsearch_summary.csv  strategy x estimator:  evaluation statistics,
                        error quantiles against the truth and the
                        difference to the full-grid legacy estimate
  mbsearch_factors.csv  as summary, per level of each task factor
"""

import csv
import glob
import json
import math
import os

import numpy as np

from .design import FACTORS

STRATEGIES = ["full", "lines", "lines_fill", "line_descent", "descent"]
ESTIMATORS = ["legacy", "local", "none", "quad", "best"]


class Grid(object):
    """The stored grid of one run, asked point by point."""

    def __init__(self, points):
        menis = sorted(set(round(p[0], 8) for p in points))
        botts = sorted(set(round(p[1], 8) for p in points))
        if len(menis) * len(botts) != len(points):
            raise ValueError("meniscus/bottom points do not form a grid")
        im = {m: i for i, m in enumerate(menis)}
        ib = {b: j for j, b in enumerate(botts)}
        self.nm, self.nb = len(menis), len(botts)
        self.meni, self.bott = menis, botts
        self.rmsd = np.empty((self.nm, self.nb))
        for m, b, r in points:
            self.rmsd[im[round(m, 8)], ib[round(b, 8)]] = r
        self.known = {}

    def __call__(self, i, j):
        if (i, j) not in self.known:
            self.known[(i, j)] = float(self.rmsd[i, j])
        return self.known[(i, j)]

    def inside(self, i, j):
        return 0 <= i < self.nm and 0 <= j < self.nb

    def best(self):
        """Best evaluated point; ties go to the first in grid order, as in
        us_fit_meniscus."""
        return min(self.known, key=lambda p: (self.known[p], p))

    def ranked(self):
        return sorted(self.known, key=lambda p: (self.known[p], p))

    def nbhd(self, p):
        return [(i, j) for i in range(p[0] - 1, p[0] + 2)
                for j in range(p[1] - 1, p[1] + 2) if self.inside(i, j)]

    def nbhd_known(self, p):
        return all(q in self.known for q in self.nbhd(p))

    def scan(self, axis, p):
        """Evaluate the line through p:  axis 0 varies the meniscus,
        axis 1 the bottom."""
        if axis == 0:
            for i in range(self.nm):
                self(i, p[1])
        else:
            for j in range(self.nb):
                self(p[0], j)

    def line_done(self, axis, p):
        if axis == 0:
            return all((i, p[1]) in self.known for i in range(self.nm))
        return all((p[0], j) in self.known for j in range(self.nb))

    def centre(self):
        return (self.nm // 2, self.nb // 2)


# --------------------------------------------------------------------------
# Strategies
# --------------------------------------------------------------------------

def descend(g, p):
    while True:
        for q in g.nbhd(p):
            g(*q)
        q = min(g.nbhd(p), key=lambda x: (g.known[x], x))
        if g.known[q] < g.known[p]:
            p = q
        else:
            return p


def _next_line(g, axis):
    """Line along axis through the best evaluated point whose line is not
    yet scanned; None if there is none."""
    for p in g.ranked():
        if not g.line_done(axis, p):
            return p
    return None


def s_full(g):
    for i in range(g.nm):
        for j in range(g.nb):
            g(i, j)


def s_lines(g):
    g.scan(0, g.centre())
    axis = 1
    while not g.nbhd_known(g.best()):
        p = _next_line(g, axis)
        if p is None:
            axis = 1 - axis
            p = _next_line(g, axis)
            if p is None:           # grid exhausted
                break
        g.scan(axis, p)
        axis = 1 - axis


def s_lines_fill(g):
    g.scan(0, g.centre())
    axis = 1
    while True:
        b = g.best()
        if g.line_done(0, b) and g.line_done(1, b):
            break
        if not g.line_done(axis, b):
            g.scan(axis, b)
        axis = 1 - axis
    descend(g, g.best())


def s_line_descent(g):
    g.scan(0, g.centre())
    descend(g, g.best())


def s_descent(g):
    descend(g, g.centre())


STRATEGY_FN = {"full": s_full, "lines": s_lines, "lines_fill": s_lines_fill,
               "line_descent": s_line_descent, "descent": s_descent}


# --------------------------------------------------------------------------
# Estimators
# --------------------------------------------------------------------------

def _weighted(g, b, c):
    f_m = f_b = w_sum = 0.0
    for q in g.nbhd(b):
        w = 1.0 / g.known[q] - c
        f_m += g.meni[q[0]] * w
        f_b += g.bott[q[1]] * w
        w_sum += w
    if w_sum > 0.0:
        return f_m / w_sum, f_b / w_sum
    return g.meni[b[0]], g.bott[b[1]]


def _parabola(x, y):
    """Vertex of the parabola through 3 equidistant points; None if it has
    no minimum."""
    curv = y[0] - 2.0 * y[1] + y[2]
    if curv <= 0.0:
        return None
    h = x[1] - x[0]
    return x[1] + 0.5 * h * (y[0] - y[2]) / curv


def _quad(g, b):
    i, j = b
    m0, b0 = g.meni[i], g.bott[j]
    hm = g.meni[1] - g.meni[0] if g.nm > 1 else 1.0
    hb = g.bott[1] - g.bott[0] if g.nb > 1 else 1.0
    nb = g.nbhd(b)
    est = None
    if len(nb) == 9:
        # r = a + bx + cy + dx^2 + exy + fy^2 in grid units
        A = np.array([[1, q[0] - i, q[1] - j, (q[0] - i) ** 2,
                       (q[0] - i) * (q[1] - j), (q[1] - j) ** 2]
                      for q in nb], float)
        y = np.array([g.known[q] for q in nb])
        c = np.linalg.lstsq(A, y, rcond=None)[0]
        H = np.array([[2 * c[3], c[4]], [c[4], 2 * c[5]]])
        if np.all(np.linalg.eigvalsh(H) > 0.0):
            est = np.linalg.solve(H, -c[1:3])
    if est is None:
        est = [0.0, 0.0]
        if 0 < i < g.nm - 1:
            v = _parabola([-1.0, 0.0, 1.0],
                          [g.known[(i + k, j)] for k in (-1, 0, 1)])
            est[0] = v if v is not None else 0.0
        if 0 < j < g.nb - 1:
            v = _parabola([-1.0, 0.0, 1.0],
                          [g.known[(i, j + k)] for k in (-1, 0, 1)])
            est[1] = v if v is not None else 0.0
    # Clamp to the in-bounds 3x3
    lo_m = -1.0 if i > 0 else 0.0
    hi_m = 1.0 if i < g.nm - 1 else 0.0
    lo_b = -1.0 if j > 0 else 0.0
    hi_b = 1.0 if j < g.nb - 1 else 0.0
    dm = min(max(float(est[0]), lo_m), hi_m)
    db = min(max(float(est[1]), lo_b), hi_b)
    return m0 + dm * hm, b0 + db * hb


def estimate(g, est):
    b = g.best()
    if est == "best":
        return g.meni[b[0]], g.bott[b[1]]
    if est == "legacy":
        return _weighted(g, b, 1.0 / max(g.known.values()))
    if est == "local":
        return _weighted(g, b, 1.0 / max(g.known[q] for q in g.nbhd(b)))
    if est == "none":
        return _weighted(g, b, 0.0)
    if est == "quad":
        return _quad(g, b)
    raise ValueError(est)


# --------------------------------------------------------------------------
# Replay
# --------------------------------------------------------------------------

def replay(run):
    """Rows (dict) of one result for every strategy."""
    pts = run["fit_mb_grid"]
    truth = run["truth"]
    t = run["task"]
    full = Grid(pts)
    s_full(full)
    gmin = float(full.rmsd.min())
    gargs = [p for p in full.known if full.known[p] == gmin]
    ref = estimate(full, "legacy")
    rows = []
    for s in STRATEGIES:
        g = Grid(pts)
        STRATEGY_FN[s](g)
        b = g.best()
        row = {"task": t["task"], "arm": run["arm"], "strategy": s,
               "grid": "%dx%d" % (g.nm, g.nb), "n_grid": g.nm * g.nb,
               "n_eval": len(g.known),
               "found_min": g.known[b] == gmin,
               "dist_min": min(max(abs(b[0] - q[0]), abs(b[1] - q[1]))
                               for q in gargs),
               "on_edge": b[0] in (0, g.nm - 1) or b[1] in (0, g.nb - 1),
               "rmsd_excess": g.known[b] / gmin - 1.0}
        for e in ESTIMATORS:
            m, bt = estimate(g, e)
            row["men_err_" + e] = m - truth["meniscus"]
            row["bot_err_" + e] = bt - truth["bottom"]
            row["men_dref_" + e] = m - ref[0]
            row["bot_dref_" + e] = bt - ref[1]
        for f in FACTORS:
            row[f] = t.get(f, "")
        row["bottom_edit_err"] = truth["edit_bottom"] - truth["bottom"]
        row["meniscus_edit_err"] = truth["edit_meniscus"] - truth["meniscus"]
        rows.append(row)
    return rows, ref


def load(outdir):
    """Result files with a grid:  OUT/<task>/<arm>.json, or the files
    given directly."""
    if os.path.isfile(outdir):
        paths = [outdir]
    else:
        paths = sorted(glob.glob(os.path.join(outdir, "*", "*.json"))) + \
            sorted(glob.glob(os.path.join(outdir, "*.json")))
    for path in paths:
        if os.path.basename(os.path.dirname(path)) == "summary":
            continue
        with open(path) as fh:
            try:
                r = json.load(fh)
            except ValueError:
                continue
        if r.get("status") == "ok" and r.get("fit_mb_grid"):
            yield path, r


def _q(v, p):
    return float(np.percentile(v, p)) if len(v) else float("nan")


def _summ(rows):
    n = np.array([r["n_eval"] for r in rows], float)
    out = {"runs": len(rows),
           "n_eval_mean": n.mean(), "n_eval_median": _q(n, 50),
           "n_eval_p95": _q(n, 95), "n_eval_max": n.max(),
           "eval_fraction": (n / np.array([r["n_grid"] for r in rows])).mean(),
           "found_min": np.mean([r["found_min"] for r in rows]),
           "dist_min_max": max(r["dist_min"] for r in rows),
           "rmsd_excess_p95": _q([r["rmsd_excess"] for r in rows], 95)}
    for e in ESTIMATORS:
        for k in ("men", "bot"):
            err = np.abs([r["%s_err_%s" % (k, e)] for r in rows])
            dref = np.abs([r["%s_dref_%s" % (k, e)] for r in rows])
            out["%s_abserr_median_%s" % (k, e)] = _q(err, 50)
            out["%s_abserr_p95_%s" % (k, e)] = _q(err, 95)
            out["%s_rmse_%s" % (k, e)] = float(np.sqrt(np.mean(err ** 2)))
            out["%s_dref_p95_%s" % (k, e)] = _q(dref, 95)
            out["%s_dref_zero_%s" % (k, e)] = float(np.mean(dref < 5e-6))
    return out


def _write(path, rows, head=None):
    if not rows:
        return
    head = head or list(rows[0].keys())
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, head, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def run(outdir, dest, check=True):
    os.makedirs(dest, exist_ok=True)
    rows = []
    mismatch = 0
    for path, r in load(outdir):
        rr, ref = replay(r)
        rows += rr
        fm = r.get("fit_mb")
        if check and fm and (abs(round(ref[0], 5) - fm["meniscus"]) > 1.5e-5
                             or abs(round(ref[1], 5) - fm["bottom"]) > 1.5e-5):
            mismatch += 1
            print("replay differs from stored fit_mb: %s (%.5f %.5f vs "
                  "%.5f %.5f)" % (path, ref[0], ref[1], fm["meniscus"],
                                  fm["bottom"]))
    if not rows:
        print("no results with a meniscus/bottom grid in %s" % outdir)
        return 1
    _write(os.path.join(dest, "mbsearch_runs.csv"), rows)

    summ = []
    for s in STRATEGIES:
        sr = [r for r in rows if r["strategy"] == s]
        summ.append(dict({"strategy": s}, **_summ(sr)))
    _write(os.path.join(dest, "mbsearch_summary.csv"), summ)

    fac = []
    for f in FACTORS + ["arm"]:
        levels = sorted(set(r[f] for r in rows), key=str)
        for lv in levels:
            for s in STRATEGIES:
                sr = [r for r in rows if r["strategy"] == s and r[f] == lv]
                if sr:
                    fac.append(dict({"factor": f, "level": lv,
                                     "strategy": s}, **_summ(sr)))
    _write(os.path.join(dest, "mbsearch_factors.csv"), fac)

    nruns = len(rows) // len(STRATEGIES)
    print("%d runs (task x arm), grid %s; replay mismatches %d -> %s"
          % (nruns, ", ".join(sorted(set(r["grid"] for r in rows))),
             mismatch, dest))
    print("%-13s %7s %7s %7s %9s   %s" % ("strategy", "n_mean", "n_p95",
                                         "n_max", "found_min",
                                         "|men err| p95 (um): "
                                         + " ".join(ESTIMATORS)))
    for s in summ:
        print("%-13s %7.1f %7.1f %7d %9.3f   %s" % (
            s["strategy"], s["n_eval_mean"], s["n_eval_p95"],
            s["n_eval_max"], s["found_min"],
            " ".join("%6.1f" % (1e4 * s["men_abserr_p95_" + e])
                     for e in ESTIMATORS)))
    return 0
