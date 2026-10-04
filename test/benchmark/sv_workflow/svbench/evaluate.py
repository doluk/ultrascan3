"""Metrics of one arm of one task against the simulated truth.

Noise.  TI and RI noise are only determined up to a common constant
(TI + c, RI - c fit equally well), and a baseline offset is a constant RI
noise, so the fitted noise is compared with the true systematic signal

    C_true(t, r) = TI_true(r) + RI_true(t) + baseline
    C_fit (t, r) = TI_fit (r) + RI_fit (t)

over the edited data:  noise_rmsd = rms(C_fit - C_true), with the shape
errors of TI (each vector centred) and of RI (each centred) and the offset
mean(C_fit - C_true) reported separately.

RMSD.  The true random noise is the noisy data minus the noise-free
simulation minus C_true; the ratio rmsd / rms(random noise) is 1 for an
ideal fit and below 1 for an over-fit.

Model.  Total concentration is the sum of the fitted signal
concentrations.  Each fitted solute is assigned to the nearest true solute
in (ln s, f/f0) (distance in units of 10 % in s and 0.2 in f/f0); solutes
farther than `ghost_distance` from every true solute count as ghosts.  Per
true solute the assigned concentration and concentration-weighted s, f/f0,
D and MW are compared with the truth.
"""

import math

import numpy as np

from .util import read_auc

S_SCALE = math.log(1.1)
K_SCALE = 0.2
GHOST_DISTANCE = 3.0


def _interp_noise(rows, x):
    if not rows:
        return np.zeros(len(x))
    arr = np.array(rows)
    return np.interp(x, arr[:, 0], arr[:, 1])


def _rms(a):
    a = np.asarray(a, dtype=float)
    return float(np.sqrt(np.mean(a * a))) if a.size else float("nan")


def noise_metrics(truth, final):
    r, t, noisy = read_auc(truth["noisy_auc"])
    _, _, clean = read_auc(truth["clean_auc"])
    left, right = truth["data_left"], truth["data_right"]

    ti_fit = next((n for n in final["noises"] if n["type"] == "ti"), None)
    ri_fit = next((n for n in final["noises"] if n["type"] == "ri"), None)

    # Radial points of the edited data:  those of the TI noise if fitted
    if ti_fit is not None:
        i0 = int(np.argmin(np.abs(r - ti_fit["minradius"])))
        idx = np.arange(i0, i0 + len(ti_fit["values"]))
    else:
        idx = np.where((r >= left - 1e-6) & (r <= right + 1e-6))[0]
    rr = r[idx]

    ti_true = _interp_noise(truth["ti_noise"], rr)
    ri_true = _interp_noise(truth["ri_noise"], t)
    c_true = ti_true[None, :] + ri_true[:, None] + truth["baseline"]

    ti_f = np.array(ti_fit["values"]) if ti_fit else np.zeros(len(rr))
    ri_f = np.array(ri_fit["values"]) if ri_fit else np.zeros(len(t))
    if len(ri_f) != len(t):
        raise ValueError("RI noise count %d != scans %d" % (len(ri_f),
                                                            len(t)))
    c_fit = ti_f[None, :] + ri_f[:, None]

    rand = noisy[:, idx] - clean[:, idx] - c_true
    diff = c_fit - c_true
    sig = clean[:, idx]
    return {
        "points": int(len(rr)), "scans": int(len(t)),
        "sigma_random_true": _rms(rand),
        "noise_rmsd": _rms(diff),
        "noise_offset": float(np.mean(diff)),
        "ti_shape_rmsd": _rms((ti_f - ti_f.mean()) - (ti_true - ti_true.mean())),
        "ri_shape_rmsd": _rms((ri_f - ri_f.mean()) - (ri_true - ri_true.mean())),
        "ti_true_rms": _rms(ti_true - ti_true.mean()),
        "ri_true_rms": _rms(ri_true - ri_true.mean()),
        "c_true_rms": _rms(c_true),
        # Data with the fitted noise removed vs. the noise-free signal
        "corrected_data_rmsd": _rms(noisy[:, idx] - c_fit - sig),
        "signal_mean": float(np.mean(sig)),
    }


def model_metrics(truth, comps):
    true = truth["solutes"]
    tot_true = sum(x["conc"] for x in true)
    tot_fit = sum(c["conc"] for c in comps)
    groups = [[] for _ in true]
    ghost = 0.0
    for c in comps:
        if c["s"] <= 0:
            ghost += c["conc"]
            continue
        dist = [math.hypot(math.log(c["s"] / x["s"]) / S_SCALE,
                           (c["ff0"] - x["ff0"]) / K_SCALE) for x in true]
        j = int(np.argmin(dist))
        if dist[j] > GHOST_DISTANCE:
            ghost += c["conc"]
        else:
            groups[j].append(c)

    species = []
    for x, grp in zip(true, groups):
        conc = sum(c["conc"] for c in grp)
        rec = {"s_true": x["s"], "ff0_true": x["ff0"], "conc_true": x["conc"],
               "conc": conc, "n_solutes": len(grp)}
        if conc > 0:
            for key in ("s", "ff0", "D", "mw"):
                val = sum(c[key] * c["conc"] for c in grp) / conc
                rec[key] = val
                rec[key + "_relerr"] = val / x[key] - 1.0
        rec["conc_relerr"] = conc / x["conc"] - 1.0
        species.append(rec)

    def worst(key):
        vals = [abs(sp[key]) for sp in species if key in sp]
        return max(vals) if vals else float("nan")

    def mean(key):
        vals = [abs(sp[key]) for sp in species if key in sp]
        return float(np.mean(vals)) if vals else float("nan")

    return {
        "total_conc_true": tot_true, "total_conc_fit": tot_fit,
        "total_conc_relerr": tot_fit / tot_true - 1.0,
        "ghost_conc_fraction": ghost / tot_fit if tot_fit > 0 else float("nan"),
        "n_solutes_fit": len(comps),
        "species_found": sum(1 for sp in species if sp["conc"] > 0),
        "species_true": len(species),
        "s_relerr_max": worst("s_relerr"), "s_relerr_mean": mean("s_relerr"),
        "ff0_relerr_max": worst("ff0_relerr"),
        "ff0_relerr_mean": mean("ff0_relerr"),
        "D_relerr_max": worst("D_relerr"), "mw_relerr_max": worst("mw_relerr"),
        "conc_relerr_max": worst("conc_relerr"),
        "conc_relerr_mean": mean("conc_relerr"),
        "species": species,
    }


def evaluate(truth, arm):
    fin = arm["final"]
    nm = noise_metrics(truth, fin)
    mm = model_metrics(truth, fin["components"])
    fm = arm["fit_mb"]
    out = {
        "rmsd": fin["rmsd"],
        "rmsd_ratio": fin["rmsd"] / nm["sigma_random_true"]
        if nm["sigma_random_true"] > 0 else float("nan"),
        "meniscus_fit_err": fm["meniscus"] - truth["meniscus"],
        "bottom_fit_err": fm["bottom"] - truth["bottom"],
        "meniscus_edit_err": truth["edit_meniscus"] - truth["meniscus"],
        "bottom_edit_err": truth["edit_bottom"] - truth["bottom"],
        "fit_mb_on_edge": fm["on_edge"],
        "edit_updated": arm["edit_updated"],
        "wall_seconds": arm["wall_seconds"],
    }
    out.update(nm)
    out.update({k: v for k, v in mm.items() if k != "species"})
    out["species"] = mm["species"]
    return out
