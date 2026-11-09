#!/usr/bin/env python3
"""
Fit sector-geometry 2D runs (misalign2d_sector.py) with the ideal 1D sector
Lamm model (s, D, c0 free per species), iso-radius mean about the rotor axis.
Multi-species runs are fitted per species.

Usage: analysis_sector.py runs/s_p0_c0.1.npz runs/s_p1_c0.1.npz ...
Writes results_sector.json (merged by file name).
"""
import sys, os, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from misalign2d_sector import SectorSolver
from misalign1d import fit_species


def analyse(fn):
    d = np.load(fn)
    a = json.loads(str(d["args"]))
    spc = np.array(d["species"])
    geo = SectorSolver([tuple(spc[0])], delta_deg=a["delta"], pivot=a["pivot"], alpha_deg=a["alpha"],
                       rpm=a["rpm"], r_m=a["r_m"], L=a["L"], Nx=a["Nx"], Ny=a["Ny"],
                       ybeta=a["ybeta"], flow=False)
    r_b = a["r_m"] + a["L"]
    r_out = geo.rc
    t = d["t"][1:]                                       # skip the first scan
    rows = []
    for k in range(len(spc)):
        C = d["C"][1:, k] / spc[k, 2]
        cb = geo.iso_radius_mean(C, r_out)
        p, rmsd = fit_species(r_out, cb, t, 1, a["r_m"], r_b, a["rpm"],
                              [(spc[k, 0], spc[k, 1], 1.0)], rmax=r_b - 0.06)
        pl = (geo.xc > 0.5 * a["L"]) & (geo.xc < 0.8 * a["L"])
        Cl = d["C"][-1, k][pl] / spc[k, 2]
        lat = np.sqrt(((Cl - Cl.mean(axis=1)[:, None]) ** 2).mean())
        rows.append(dict(file=fn, species=k, delta=a["delta"], c0=float(spc[:, 2].sum()), rpm=a["rpm"],
                         L=a["L"], Nx=a["Nx"], Ny=a["Ny"], s=p[0, 0], D=p[0, 1], c_fit=p[0, 2],
                         rmsd=rmsd, lateral_rms=lat))
        print(f"{fn:26s} sp{k} delta={a['delta']:.1f} c0={rows[-1]['c0']:.2g} | s={p[0,0]:.4f} "
              f"D={p[0,1]:.4f} c={p[0,2]:.4f} rmsd={rmsd:.1e} lat.rms={lat:.1e}", flush=True)
    return rows


if __name__ == "__main__":
    rows = [r for f in sys.argv[1:] for r in analyse(f)]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results_sector.json")
    try:
        old = {(r["file"], r["species"]): r for r in json.load(open(out))}
    except (OSError, ValueError):
        old = {}
    old.update({(r["file"], r["species"]): r for r in rows})
    json.dump(list(old.values()), open(out, "w"), indent=1)
