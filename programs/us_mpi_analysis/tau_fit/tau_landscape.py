#!/usr/bin/env python3
"""Evaluate us_mpi_analysis 2DSA meniscus / radial-scan-time (tau) fits.

Reads one or more analysis-results.tar (or directories with their unpacked
files), takes the RMSD of every (meniscus, tau) point from
analysis_files.txt, and reports per dataset (triple):

  * the RMSD landscape over meniscus x tau,
  * the grid minimum and the optimum refined by parabolas through the tau
    profile (RMSD^2 minimised over the meniscus) and the meniscus profile,
  * with --sigma and --points: profile 1-sigma uncertainties from
    delta-chi^2 = 1, chi^2 = points * RMSD^2 / sigma^2,
  * the signal-weighted s, f/f0 and MW of the model at the grid minimum.

--joint fits the triples of one cell and channel (e.g. 4B220 and 4B230: the
same solution column at several wavelengths) with one common meniscus and a
radial scan time of their own:

    chi^2(m, tau_1, tau_2, ...) = sum_i chi^2_i(m, tau_i)

For every meniscus of the common grid each triple takes its best tau (a
parabola through the tau column); the sum gives the joint meniscus profile,
whose minimum is the common meniscus.  The tau of every triple is reported
at that meniscus, with its range over the meniscus uncertainty.  Without
--sigma each triple's sigma is its own lowest RMSD; without --points the
profile is relative and no uncertainties are given.

--sigma and --points take one value for all triples or TRIPLE=VALUE (repeat
the option), e.g. --points 4B220=52000 --points 4B230=51000.

With matplotlib installed, --plot writes the figures.

    tau_landscape.py chB-4B220/output/analysis-results.tar \\
        chB-4B230/output/analysis-results.tar --joint --points 52000 --plot
"""

import argparse
import math
import re
import sys
import tarfile
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path


def read_results(source):
    """Return {name: text} of analysis_files.txt and the model files."""
    files = {}
    src = Path(source)
    if src.is_dir():
        for path in src.iterdir():
            if path.name == "analysis_files.txt" or path.name.endswith(".model.xml"):
                files[path.name] = path.read_text(errors="replace")
    else:
        with tarfile.open(src) as tar:
            for member in tar.getmembers():
                name = Path(member.name).name
                if name == "analysis_files.txt" or name.endswith(".model.xml"):
                    files[name] = tar.extractfile(member).read().decode(errors="replace")
    if "analysis_files.txt" not in files:
        sys.exit("no analysis_files.txt in %s" % source)
    return files


def parse_points(text):
    """Fit points per triple: list of dict(file, meniscus, tau, rmsd)."""
    points = defaultdict(list)
    for line in text.splitlines():
        if not line.strip():
            continue
        fields = line.split(";")
        name = fields[0]
        vals = dict(f.split("=", 1) for f in fields[1:] if "=" in f)
        if not name.endswith(".model.xml") or "variance" not in vals:
            continue
        triple = name.split(".")[2] if name.count(".") >= 3 else "?"
        points[triple].append(dict(file=name,
                                   meniscus=float(vals.get("meniscus_value", "nan")),
                                   tau=float(vals.get("tau_value", "0")),
                                   rmsd=math.sqrt(max(0.0, float(vals["variance"])))))
    return points


def parabola(xs, ys):
    """Vertex x0, curvature a and vertex value y0 (y = a (x - x0)^2 + y0)
    through the minimum and its neighbours, and an edge flag.  At an edge,
    or without curvature, the grid minimum is returned with a = None."""
    i = min(range(len(ys)), key=ys.__getitem__)
    if len(ys) < 3 or i == 0 or i == len(ys) - 1:
        return xs[i], None, ys[i], len(ys) >= 3
    h1, h2 = xs[i] - xs[i - 1], xs[i + 1] - xs[i]
    if abs(h1 - h2) > 1e-6 * max(h1, h2):
        return xs[i], None, ys[i], False
    y1, y2, y3 = ys[i - 1], ys[i], ys[i + 1]
    den = y1 - 2 * y2 + y3
    if den <= 0:
        return xs[i], None, ys[i], False
    x0 = xs[i] + 0.5 * h1 * (y1 - y3) / den
    a = den / (2 * h1 * h1)
    return x0, a, y2 - a * (x0 - xs[i]) ** 2, False


def model_moments(text):
    """Signal-weighted s (S), f/f0 and MW of a model XML."""
    root = ET.fromstring(text)
    tot = ss = kk = mw = 0.0
    for an in root.iter("analyte"):
        c = float(an.get("signal", "0"))
        tot += c
        ss += c * float(an.get("s", "0"))
        kk += c * float(an.get("f_f0", "0"))
        mw += c * float(an.get("mw", "0"))
    if tot <= 0:
        return None
    return dict(total=tot, s=ss / tot * 1e13, ff0=kk / tot, mw=mw / tot)


def per_triple(specs, kind):
    """Parse VALUE / TRIPLE=VALUE options into (default, {triple: value})."""
    default, values = None, {}
    for spec in specs or []:
        key, sep, val = spec.partition("=")
        try:
            if sep:
                values[key] = kind(val)
            else:
                default = kind(spec)
        except ValueError:
            sys.exit("invalid value %s" % spec)
    return default, values


class Landscape:
    """The fit points of one triple on its meniscus x tau grid."""

    def __init__(self, triple, pts, files):
        self.triple = triple
        self.pts = pts
        self.files = files
        self.mens = sorted({round(p["meniscus"], 6) for p in pts})
        self.taus = sorted({round(p["tau"], 6) for p in pts})
        self.grid = {(round(p["meniscus"], 6), round(p["tau"], 6)): p for p in pts}
        self.best = min(pts, key=lambda p: p["rmsd"])

    def rmsd(self, m, t):
        p = self.grid.get((m, t))
        return p["rmsd"] if p else float("nan")

    def tau_column(self, m):
        """Best tau and RMSD^2 at meniscus m (parabola through the column)."""
        taus = [t for t in self.taus if (m, t) in self.grid]
        r2 = [self.grid[(m, t)]["rmsd"] ** 2 for t in taus]
        t0, a, y0, edge = parabola(taus, r2)
        return t0, y0, a, edge


def report_triple(ls, sigma, points, plot):
    mens, taus, grid = ls.mens, ls.taus, ls.grid
    print("== %s: %d fit points, meniscus %d x tau %d" % (ls.triple, len(ls.pts), len(mens), len(taus)))
    print("   RMSD (OD)  rows: tau (s/point), columns: meniscus (cm)")
    print("   %9s " % "" + " ".join("%9.4f" % m for m in mens))
    for t in taus:
        print("   %9.4f " % t + " ".join("%9.6f" % grid[(m, t)]["rmsd"] if (m, t) in grid else "%9s" % "-"
                                      for m in mens))

    best = ls.best
    print("   grid minimum:   meniscus %.4f cm, tau %.4f s/point, RMSD %.6f OD  (%s)"
          % (best["meniscus"], best["tau"], best["rmsd"], best["file"]))

    # Profiles (RMSD^2 minimised over the other parameter) and parabolas
    prof_t = [min(grid[(m, t)]["rmsd"] ** 2 for m in mens if (m, t) in grid) for t in taus]
    t_opt, t_a, _, t_edge = parabola(taus, prof_t)
    row = round(best["tau"], 6)
    prof_m = ([grid[(m, row)]["rmsd"] ** 2 for m in mens] if t_edge else
              [min(grid[(m, t)]["rmsd"] ** 2 for t in taus if (m, t) in grid) for m in mens])
    m_opt, m_a, _, m_edge = parabola(mens, prof_m)
    note = []
    if t_edge:
        note.append("tau at the grid edge")
    if m_edge:
        note.append("meniscus at the grid edge")
    print("   refined:        meniscus %.5f cm, tau %.5f s/point%s"
          % (m_opt, t_opt, ("  [" + "; ".join(note) + ": widen the range]") if note else ""))

    if sigma and points:
        scale = points / sigma ** 2
        sd = lambda curv: (1.0 / math.sqrt(curv * scale)) if curv else float("nan")
        print("   profile 1-sigma (delta chi^2 = 1, sigma %.4g OD, %d points):  meniscus %.2g cm, tau %.2g s/point"
              % (sigma, points, sd(m_a), sd(t_a)))

    mom = model_moments(ls.files[best["file"]]) if best["file"] in ls.files else None
    if mom:
        print("   model at the grid minimum: total %.4f, <s> %.4f S, <f/f0> %.4f, <MW> %.0f"
              % (mom["total"], mom["s"], mom["ff0"], mom["mw"]))
    zero = [p for p in ls.pts if p["tau"] == 0.0 and round(p["meniscus"], 6) == round(best["meniscus"], 6)]
    if zero and zero[0]["file"] in ls.files and best["tau"] != 0.0:
        mz = model_moments(ls.files[zero[0]["file"]])
        if mz and mom:
            print("   same meniscus, tau = 0:     total %.4f, <s> %.4f S, <f/f0> %.4f, <MW> %.0f  (RMSD %.6f)"
                  % (mz["total"], mz["s"], mz["ff0"], mz["mw"], zero[0]["rmsd"]))

    if plot:
        plt = pyplot()
        if plt is None:
            return
        z = [[grid[(m, t)]["rmsd"] if (m, t) in grid else float("nan") for m in mens] for t in taus]
        fig, ax = plt.subplots(1, 2, figsize=(12, 4.5), constrained_layout=True)
        if len(mens) > 1 and len(taus) > 1:
            cf = ax[0].contourf(mens, taus, z, levels=20, cmap="Blues_r")
            fig.colorbar(cf, ax=ax[0]).set_label("RMSD (OD)")
        else:
            ax[0].plot(mens if len(taus) == 1 else taus, [r for row_ in z for r in row_], "o-")
        ax[0].plot(m_opt, t_opt, "o", mfc="none", mec="#eb6834", mew=2, ms=9, label="refined optimum")
        ax[0].set_xlabel("meniscus (cm)")
        ax[0].set_ylabel("tau (s/point)")
        ax[0].legend(frameon=False)
        ax[0].set_title("%s: RMSD landscape" % ls.triple, loc="left")
        ax[1].plot(taus, [math.sqrt(v) for v in prof_t], "o-", color="#2a78d6")
        ax[1].set_xlabel("tau (s/point)")
        ax[1].set_ylabel("RMSD, meniscus optimised (OD)")
        ax[1].set_title("tau profile", loc="left")
        out = "%s_tau_landscape.png" % ls.triple
        fig.savefig(out, dpi=130)
        print("   wrote %s" % out)


def pyplot():
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        return plt
    except ImportError:
        print("   (matplotlib not available: no plot)")
        return None


def interp(xs, ys, x):
    """Linear interpolation (clamped) of ys over increasing xs."""
    if x <= xs[0]:
        return ys[0]
    for k in range(1, len(xs)):
        if x <= xs[k]:
            f = (x - xs[k - 1]) / (xs[k] - xs[k - 1])
            return ys[k - 1] + f * (ys[k] - ys[k - 1])
    return ys[-1]


def report_joint(group, lss, sigmas, npoints, plot):
    """Common meniscus, own tau per triple."""
    names = [ls.triple for ls in lss]
    common = sorted(set.intersection(*[set(ls.mens) for ls in lss]))
    print("== joint %s: %s, common meniscus, tau per triple" % (group, ", ".join(names)))
    if len(common) < 3:
        print("   fewer than 3 common meniscus values: run the triples on the same meniscus grid")
        return
    relative = any(npoints[n] is None for n in names)
    rows = []
    for m in common:
        chi, row = 0.0, []
        for ls in lss:
            t0, y0, a, edge = ls.tau_column(m)
            n = npoints[ls.triple] or 1
            chi += n * y0 / sigmas[ls.triple] ** 2
            row.append((t0, math.sqrt(max(y0, 0.0)), a, edge))
        rows.append((m, chi, row))
    chi_min = min(r[1] for r in rows)

    print("   meniscus (cm)  " + "  ".join("%-24s" % ("%s: tau, RMSD" % n) for n in names)
          + ("  sum of (RMSD/sigma)^2 - min" if relative else "  delta chi^2"))
    for m, chi, row in rows:
        cells = "  ".join("%-24s" % ("%7.4f%s %.6f" % (t0, "*" if edge else " ", r)) for t0, r, _, edge in row)
        print("   %9.4f      %s  %.5g" % (m, cells, chi - chi_min))
    if any(edge for _, _, row in rows for _, _, _, edge in row):
        print("   (* tau at the edge of the tau grid)")

    m_opt, m_a, _, m_edge = parabola([r[0] for r in rows], [r[1] for r in rows])
    taus_m = {n: [r[2][k][0] for r in rows] for k, n in enumerate(names)}
    ms = [r[0] for r in rows]
    print("   common meniscus: %.5f cm%s" % (m_opt, "  [at the edge of the common meniscus grid: widen the range]"
                                             if m_edge else ""))
    sd_m = None
    if m_a and not relative:
        sd_m = 1.0 / math.sqrt(m_a)
        print("   meniscus 1-sigma (joint profile, delta chi^2 = 1): %.2g cm" % sd_m)
    for k, n in enumerate(names):
        t_at = interp(ms, taus_m[n], m_opt)
        text = "   %s: tau %.4f s/point at the common meniscus" % (n, t_at)
        if sd_m:
            lo, hi = sorted((interp(ms, taus_m[n], m_opt - sd_m), interp(ms, taus_m[n], m_opt + sd_m)))
            text += " (%.4f to %.4f over meniscus +- 1 sigma)" % (lo, hi)
        print(text)
    sig_txt = ", ".join("%s %.4g" % (n, sigmas[n]) for n in names)
    print("   sigma: %s OD%s" % (sig_txt, "" if relative else "; points: " + ", ".join(
        "%s %d" % (n, npoints[n]) for n in names)))
    if relative:
        print("   (no --points: the profile is relative, no uncertainties)")

    if plot:
        plt = pyplot()
        if plt is None:
            return
        fig, ax = plt.subplots(1, 2, figsize=(12, 4.5), constrained_layout=True)
        ax[0].plot(ms, [r[1] - chi_min for r in rows], "o-", color="#2a78d6")
        ax[0].axvline(m_opt, color="#eb6834", lw=1)
        ax[0].set_xlabel("meniscus (cm)")
        ax[0].set_ylabel("relative sum" if relative else "delta chi^2")
        ax[0].set_title("%s: joint meniscus profile" % group, loc="left")
        for n in names:
            ax[1].plot(ms, taus_m[n], "o-", label=n)
        ax[1].axvline(m_opt, color="#eb6834", lw=1)
        ax[1].set_xlabel("meniscus (cm)")
        ax[1].set_ylabel("best tau (s/point)")
        ax[1].legend(frameon=False)
        ax[1].set_title("best tau per triple", loc="left")
        out = "%s_joint_tau.png" % group
        fig.savefig(out, dpi=130)
        print("   wrote %s" % out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", nargs="+", help="analysis-results.tar files or directories with their files")
    ap.add_argument("--sigma", action="append", help="noise sigma (OD): VALUE or TRIPLE=VALUE")
    ap.add_argument("--points", action="append",
                    help="number of fitted data points (scans x radii): VALUE or TRIPLE=VALUE")
    ap.add_argument("--joint", action="store_true",
                    help="common meniscus, own tau, for the triples of each cell and channel")
    ap.add_argument("--plot", action="store_true", help="write the figures (needs matplotlib)")
    a = ap.parse_args()

    sig_default, sig_by = per_triple(a.sigma, float)
    pts_default, pts_by = per_triple(a.points, int)

    landscapes = {}
    for source in a.results:
        files = read_results(source)
        for triple, pts in parse_points(files["analysis_files.txt"]).items():
            if triple in landscapes:
                print("warning: %s given more than once; using %s" % (triple, source))
            landscapes[triple] = Landscape(triple, pts, files)

    for triple in sorted(landscapes):
        report_triple(landscapes[triple], sig_by.get(triple, sig_default), pts_by.get(triple, pts_default), a.plot)

    if a.joint:
        groups = defaultdict(list)
        for triple, ls in sorted(landscapes.items()):
            m = re.match(r"^(\d+[A-Za-z])", triple)
            groups[m.group(1) if m else triple].append(ls)
        for group, lss in sorted(groups.items()):
            if len(lss) < 2:
                print("== joint %s: only one triple (%s)" % (group, lss[0].triple))
                continue
            sigmas = {ls.triple: sig_by.get(ls.triple, sig_default) or ls.best["rmsd"] for ls in lss}
            npoints = {ls.triple: pts_by.get(ls.triple, pts_default) for ls in lss}
            report_joint(group, lss, sigmas, npoints, a.plot)
    return 0


if __name__ == "__main__":
    sys.exit(main())
