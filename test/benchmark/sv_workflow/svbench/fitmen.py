"""Re-implementation of the us_fit_meniscus steps used by the workflow.

Meniscus+bottom fit (US_FitMeniscus::plot_3d):  the grid point with the
lowest RMSD and its (up to 8) grid neighbours are averaged with weights
1/rmsd - 1/rmsd_max, giving the fitted meniscus and bottom (reported with
5 decimals, as us_fit_meniscus does).

Edit update (US_FitMeniscus::edit_update):  the meniscus and bottom
values of the edit are replaced; the update is refused when the new
meniscus is not left of the data range.
"""

import math
import re


def fit_meniscus_bottom(points):
    """points: list of (meniscus, bottom, rmsd) of a meniscus+bottom grid,
    in the order written by us_mpi_analysis (meniscus-major).
    Returns dict with fitted and best-grid meniscus/bottom."""
    pts = sorted(points, key=lambda p: (p[0], p[1]))
    menis = sorted(set(round(p[0], 8) for p in pts))
    botts = sorted(set(round(p[1], 8) for p in pts))
    nmeni, nbott = len(menis), len(botts)
    if nmeni * nbott != len(pts):
        raise ValueError("meniscus/bottom points do not form a grid")

    rmsd = [p[2] for p in pts]
    ix_best = min(range(len(pts)), key=lambda i: rmsd[i])
    min_r = 1.0 / max(rmsd)
    ix_men, ix_bot = ix_best // nbott, ix_best % nbott

    f_meni = f_bott = w_sum = 0.0
    for jm in range(ix_men - 1, ix_men + 2):
        if jm < 0 or jm >= nmeni:
            continue
        for jb in range(ix_bot - 1, ix_bot + 2):
            if jb < 0 or jb >= nbott:
                continue
            jx = jm * nbott + jb
            w = 1.0 / rmsd[jx] - min_r
            f_meni += pts[jx][0] * w
            f_bott += pts[jx][1] * w
            w_sum += w
    if w_sum > 0.0:
        f_meni /= w_sum
        f_bott /= w_sum
    else:                       # all RMSDs equal
        f_meni, f_bott = pts[ix_best][0], pts[ix_best][1]

    return {"meniscus": round(f_meni, 5), "bottom": round(f_bott, 5),
            "best_meniscus": pts[ix_best][0],
            "best_bottom": pts[ix_best][1],
            "best_rmsd": rmsd[ix_best],
            "on_edge": ix_men in (0, nmeni - 1) or ix_bot in (0, nbott - 1)}


def points_from_models(models):
    """(meniscus, bottom, rmsd) of the models of a meniscus+bottom fit;
    rmsd = sqrt(variance) as in us_fit_meniscus."""
    return [(m["meniscus"], m["bottom"], math.sqrt(m["variance"]))
            for m in models]


def _attr(text, tag, attr):
    m = re.search(r'<%s\s[^>]*%s="([^"]*)"' % (tag, attr), text)
    return float(m.group(1)) if m else None


def update_edit(src, dst, meniscus, bottom):
    """Write the edit src to dst with new meniscus and bottom values.
    Returns False (and writes nothing) if the meniscus would reach into
    the data range, as us_fit_meniscus refuses that update."""
    with open(src) as fh:
        text = fh.read()
    left = _attr(text, "data_range", "left")
    if meniscus >= left:
        return False
    text = re.sub(r'(<meniscus\s+radius=")[^"]*(")',
                  r"\g<1>%.5f\g<2>" % meniscus, text, count=1)
    text = re.sub(r'(<bottom\s+radius=")[^"]*(")',
                  r"\g<1>%.5f\g<2>" % bottom, text, count=1)
    with open(dst, "w") as fh:
        fh.write(text)
    return True
