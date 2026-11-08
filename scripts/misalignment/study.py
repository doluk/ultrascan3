#!/usr/bin/env python3
"""Parameter study: apparent s, D and aggregate fraction obtained when data
from a misaligned cell (closure model, eq. 1 of misalign1d.py) are analysed
with the ideal (aligned) Lamm model.  Writes results.json and figures."""
import json
import numpy as np
from misalign1d import MisalignGeometry, simulate, fit_species, DEG
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

R_M, R_B, RPM, ALPHA, RP = 6.0, 7.2, 50000., 1.25, 6.5
TIMES = np.arange(600., 10801., 600.)
MONO = (4.4e-13, 6.0e-7)
DIMER = (6.8e-13, 4.6e-7)
DELTAS = [0.0, 0.25, 0.5, 1.0, 2.0, 4.0]
KAPPAS = [1.0, 0.1]


def run_single(delta, kappa):
    g = MisalignGeometry(delta, ALPHA, RP)
    r, C, _ = simulate([(*MONO, 1.0)], TIMES, R_M, R_B, RPM, g, kappa, N=1200)
    p, rmsd = fit_species(r, C, TIMES, 1, R_M, R_B, RPM, [(*MONO, 1.0)])
    return dict(delta=delta, kappa=kappa, s=p[0, 0], D=p[0, 1], c0=p[0, 2],
                rmsd=rmsd), r, C


def run_mix(delta, kappa, fd=0.15):
    g = MisalignGeometry(delta, ALPHA, RP)
    sp = [(*MONO, 1 - fd), (*DIMER, fd)]
    r, C, _ = simulate(sp, TIMES, R_M, R_B, RPM, g, kappa, N=1200)
    p, rmsd = fit_species(r, C, TIMES, 2, R_M, R_B, RPM, sp)
    return dict(delta=delta, kappa=kappa, s1=p[0, 0], D1=p[0, 1],
                s2=p[1, 0], D2=p[1, 1], f2=p[1, 2] / (p[0, 2] + p[1, 2]),
                rmsd=rmsd)


def _job(args):
    d, kap = args
    rs, r, C = run_single(d, kap)
    rm = run_mix(d, kap)
    print(f"kappa={kap:4.2f} delta={d:4.2f}  s={rs['s']:.4f}S D={rs['D']:.4f}e-7 rmsd={rs['rmsd']:.2e}"
          f" | mix s1={rm['s1']:.3f} s2={rm['s2']:.3f} f2={rm['f2']:.4f} rmsd={rm['rmsd']:.1e}", flush=True)
    return rs, rm, r, C


if __name__ == "__main__":
    from multiprocessing import Pool
    jobs = [(d, k) for k in KAPPAS for d in DELTAS if not (d == 0 and k != KAPPAS[0])]
    with Pool(4) as pool:
        out = pool.map(_job, jobs)
    res = dict(single=[o[0] for o in out], mix=[o[1] for o in out])
    profiles = {j: (o[2], o[3]) for j, o in zip(jobs, out)}
    json.dump(res, open("results.json", "w"), indent=1)

    # figure 1: profiles aligned vs 1 and 4 deg (kappa = 1)
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    r0, C0 = profiles[(0.0, 1.0)]
    for j, d in enumerate([1.0, 4.0]):
        r, C = profiles[(d, 1.0)]
        for i in range(0, len(TIMES), 3):
            ax[j].plot(r0, C0[i], "k-", lw=0.8, label="aligned" if i == 0 else None)
            ax[j].plot(r, C[i], "r--", lw=0.8, label=f"misaligned {d}°, κ=1" if i == 0 else None)
        ax[j].set_xlabel("r (cm)"); ax[j].set_ylabel("c (rel.)"); ax[j].set_ylim(0, 1.3)
        ax[j].legend(loc="upper left")
    plt.tight_layout(); plt.savefig("fig_profiles.png", dpi=110)

    # figure 2: apparent s, D, f2 vs delta
    fig, ax = plt.subplots(1, 3, figsize=(13, 3.6))
    for kap, mk in zip(KAPPAS, ["o-", "s--"]):
        S = [x for x in res["single"] if x["kappa"] == kap or x["delta"] == 0]
        M = [x for x in res["mix"] if x["kappa"] == kap or x["delta"] == 0]
        d = [x["delta"] for x in S]
        ax[0].plot(d, [100 * (x["s"] / 4.4 - 1) for x in S], mk, label=f"κ={kap}")
        ax[1].plot(d, [100 * (x["D"] / 6.0 - 1) for x in S], mk, label=f"κ={kap}")
        ax[2].plot([x["delta"] for x in M], [100 * x["f2"] for x in M], mk, label=f"κ={kap}")
    ax[0].set_ylabel("Δs/s (%)"); ax[1].set_ylabel("ΔD/D (%)"); ax[2].set_ylabel("apparent dimer fraction (%)")
    ax[2].axhline(15, color="k", lw=0.5)
    for a in ax:
        a.set_xlabel("misalignment δ (deg)"); a.legend()
    plt.tight_layout(); plt.savefig("fig_bias.png", dpi=110)
