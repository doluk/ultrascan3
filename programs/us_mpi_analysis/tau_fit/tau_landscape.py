#!/usr/bin/env python3
"""Evaluate a us_mpi_analysis 2DSA meniscus / radial-scan-time (tau) fit.

Reads analysis-results.tar (or a directory with its unpacked files), takes
the RMSD of every (meniscus, tau) point from analysis_files.txt, and reports
per dataset (triple):

  * the RMSD landscape over meniscus x tau,
  * the grid minimum and the optimum refined by parabolas through the tau
    profile (RMSD^2 minimised over the meniscus) and the meniscus profile,
  * with --sigma and --points: profile 1-sigma uncertainties from
    delta-chi^2 = 1, chi^2 = points * RMSD^2 / sigma^2,
  * the signal-weighted s, f/f0 and MW of the model at the grid minimum.

With matplotlib installed, --plot writes a landscape figure per triple.

    tau_landscape.py analysis-results.tar [--sigma 0.004 --points 25000] [--plot]
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
    """Vertex and curvature a (y = a (x - x0)^2 + y0) through the minimum and
    its neighbours; (x_min, None, True) when the minimum is at an edge."""
    i = min(range(len(ys)), key=ys.__getitem__)
    if i == 0 or i == len(ys) - 1:
        return xs[i], None, True
    h1, h2 = xs[i] - xs[i - 1], xs[i + 1] - xs[i]
    if abs(h1 - h2) > 1e-9 * max(h1, h2):
        return xs[i], None, False
    y1, y2, y3 = ys[i - 1], ys[i], ys[i + 1]
    den = y1 - 2 * y2 + y3
    if den <= 0:
        return xs[i], None, False
    return xs[i] + 0.5 * h1 * (y1 - y3) / den, den / (2 * h1 * h1), False


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", help="analysis-results.tar or a directory with its files")
    ap.add_argument("--sigma", type=float, help="noise sigma (OD) for profile uncertainties")
    ap.add_argument("--points", type=int, help="number of fitted data points (scans x radii) for profile uncertainties")
    ap.add_argument("--plot", action="store_true", help="write <triple>_tau_landscape.png (needs matplotlib)")
    a = ap.parse_args()

    files = read_results(a.results)
    for triple, pts in sorted(parse_points(files["analysis_files.txt"]).items()):
        mens = sorted({round(p["meniscus"], 6) for p in pts})
        taus = sorted({round(p["tau"], 6) for p in pts})
        grid = {(round(p["meniscus"], 6), round(p["tau"], 6)): p for p in pts}
        print("== %s: %d fit points, meniscus %d x tau %d" % (triple, len(pts), len(mens), len(taus)))

        # Landscape table
        print("   RMSD (OD)  rows: tau (s/point), columns: meniscus (cm)")
        print("   %9s " % "" + " ".join("%9.4f" % m for m in mens))
        for t in taus:
            print("   %9.4f " % t + " ".join("%9.6f" % grid[(m, t)]["rmsd"] if (m, t) in grid else "%9s" % "-"
                                          for m in mens))

        best = min(pts, key=lambda p: p["rmsd"])
        print("   grid minimum:   meniscus %.4f cm, tau %.4f s/point, RMSD %.6f OD  (%s)"
              % (best["meniscus"], best["tau"], best["rmsd"], best["file"]))

        # Profiles (RMSD^2 minimised over the other parameter) and parabolas
        prof_t = [min(grid[(m, t)]["rmsd"] ** 2 for m in mens if (m, t) in grid) for t in taus]
        t_opt, t_a, t_edge = parabola(taus, prof_t) if len(taus) > 2 else (best["tau"], None, True)
        row = round(best["tau"], 6)
        prof_m = ([grid[(m, row)]["rmsd"] ** 2 for m in mens] if t_edge else
                  [min(grid[(m, t)]["rmsd"] ** 2 for t in taus if (m, t) in grid) for m in mens])
        m_opt, m_a, m_edge = parabola(mens, prof_m) if len(mens) > 2 else (best["meniscus"], None, True)
        note = []
        if t_edge and len(taus) > 2:
            note.append("tau at the grid edge")
        if m_edge and len(mens) > 2:
            note.append("meniscus at the grid edge")
        print("   refined:        meniscus %.5f cm, tau %.5f s/point%s"
              % (m_opt, t_opt, ("  [" + "; ".join(note) + ": widen the range]") if note else ""))

        if a.sigma and a.points:
            scale = a.points / a.sigma ** 2
            sd = lambda curv: (1.0 / math.sqrt(curv * scale)) if curv else float("nan")
            print("   profile 1-sigma (delta chi^2 = 1, sigma %.4g OD, %d points):  meniscus %.2g cm, tau %.2g s/point"
                  % (a.sigma, a.points, sd(m_a), sd(t_a)))

        mom = model_moments(files[best["file"]]) if best["file"] in files else None
        if mom:
            print("   model at the grid minimum: total %.4f, <s> %.4f S, <f/f0> %.4f, <MW> %.0f"
                  % (mom["total"], mom["s"], mom["ff0"], mom["mw"]))
        zero = [p for p in pts if p["tau"] == 0.0 and round(p["meniscus"], 6) == round(best["meniscus"], 6)]
        if zero and zero[0]["file"] in files and best["tau"] != 0.0:
            mz = model_moments(files[zero[0]["file"]])
            if mz and mom:
                print("   same meniscus, tau = 0:     total %.4f, <s> %.4f S, <f/f0> %.4f, <MW> %.0f  (RMSD %.6f)"
                      % (mz["total"], mz["s"], mz["ff0"], mz["mw"], zero[0]["rmsd"]))

        if a.plot:
            try:
                import matplotlib
                matplotlib.use("Agg")
                import matplotlib.pyplot as plt
            except ImportError:
                print("   (matplotlib not available: no plot)")
                continue
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
            ax[0].set_title("%s: RMSD landscape" % triple, loc="left")
            ax[1].plot(taus, [math.sqrt(v) for v in prof_t], "o-", color="#2a78d6")
            ax[1].set_xlabel("tau (s/point)")
            ax[1].set_ylabel("RMSD, meniscus optimised (OD)")
            ax[1].set_title("tau profile", loc="left")
            out = "%s_tau_landscape.png" % triple
            fig.savefig(out, dpi=130)
            print("   wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
