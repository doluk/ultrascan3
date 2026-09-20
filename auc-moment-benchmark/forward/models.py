"""
The frozen ground-truth test set (spec A.3) plus the hydrodynamics needed to
turn (s, f/f0) into a physically consistent D.

Everything here is plain numpy so the handoff package carries no UltraScan or
Qt dependency.  The same models are defined in generate/models.inc for the
UltraScan Layer-1 generator, and the two must agree.
"""

import numpy as np

from lamm import LammGrid, simulate_model, solve_species, omega

# Water at 20 C, the standard s20,w reference condition
ETA_20W = 0.0100194      # g / (cm s)
RHO_20W = 0.998234       # g / mL
N_A = 6.02214076e23
K_B = 1.380649e-16       # erg / K
T_20 = 293.15            # K
SVEDBERG = 1e-13


def d_from_s(s, f_f0=1.25, vbar20=0.73):
    """
    Diffusion coefficient consistent with a given s and frictional ratio.

    From the Svedberg relation  f = M (1 - vbar rho) / (N_A s)  and
    f = (f/f0) * 6 pi eta (3 M vbar / (4 pi N_A))^(1/3):

        M^(2/3) = (f/f0) 6 pi eta (3 vbar / (4 pi N_A))^(1/3) N_A s / (1 - vbar rho)

    then D = kT / f.
    """
    buoy = 1.0 - vbar20 * RHO_20W
    c = f_f0 * 6.0 * np.pi * ETA_20W * (3.0 * vbar20 / (4.0 * np.pi * N_A)) ** (1.0 / 3.0)
    m23 = c * N_A * s / buoy
    M = m23 ** 1.5
    f = M * buoy / (N_A * s)
    return K_B * T_20 / f, M


def species(s_svedberg, conc, f_f0=1.25, vbar20=0.73):
    s = s_svedberg * SVEDBERG
    D, M = d_from_s(s, f_f0, vbar20)
    return {"s": s, "D": D, "c": float(conc), "f_f0": f_f0, "mw": M}


# --------------------------------------------------------------------------
# Run geometry — realistic standard-cell, 45 krpm, ~100 scans
# --------------------------------------------------------------------------

class RunGeometry:
    def __init__(self, meniscus=5.90, bottom=7.20, rpm=45000,
                 n_scans=100, t_start=600.0, t_end=22000.0,
                 radial_resolution=1.0e-3, band=False, band_volume=0.015):
        self.meniscus = meniscus
        self.bottom = bottom
        self.rpm = rpm
        self.n_scans = n_scans
        self.times = np.linspace(t_start, t_end, n_scans)
        self.radial_resolution = radial_resolution
        # Band-forming (zonal) run: sample layered as a lamella at the
        # meniscus rather than filling the cell.  See forward/band.py.
        self.band = bool(band)
        self.band_volume = float(band_volume)
        # measured radial grid, odd count so Simpson is exact-order
        n = int(np.floor((bottom - meniscus) / radial_resolution)) + 1
        if n % 2 == 0:
            n += 1
        self.r = np.linspace(meniscus, bottom, n)

    @property
    def omega2(self):
        return omega(self.rpm) ** 2


# --------------------------------------------------------------------------
# Test set
# --------------------------------------------------------------------------

def _lognormal_components(s_mid, rel_width, total_c, n=41, f_f0=1.25):
    """Discretise a log-normal peak into many closely spaced species."""
    sigma = rel_width
    lo, hi = np.log(s_mid) - 4 * sigma, np.log(s_mid) + 4 * sigma
    ls = np.linspace(lo, hi, n)
    w = np.exp(-0.5 * ((ls - np.log(s_mid)) / sigma) ** 2)
    w /= w.sum()
    return [species(np.exp(l), total_c * wi, f_f0) for l, wi in zip(ls, w)]


def test_set():
    """
    Returns {model_id: dict} with keys:
      components : list of species dicts (the ground truth atoms)
      atomic     : whether the truth really is a finite sum of Diracs
      reaction   : None, or dict describing a rapid monomer-dimer equilibrium
      note       : what the model is for
    """
    C = 0.5   # total loading, OD or fringes

    ms = {}
    ms["S1"] = dict(
        components=[species(4.0, C)], atomic=True, reaction=None,
        note="single species; sigma(t) calibration and Gate G0/G1",
    )
    for tag, ds in (("15", 0.15), ("10", 0.10), ("05", 0.05), ("03", 0.03)):
        ms[f"M2-{tag}"] = dict(
            components=[species(4.0, C / 2), species(4.0 * (1 + ds), C / 2)],
            atomic=True, reaction=None,
            note=f"two species, delta-s/s = {ds:.0%}; resolution crossover",
        )
    ms["M3"] = dict(
        # monomer / dimer / trimer at constant density and shape: s ~ n^(2/3)
        components=[species(4.0, C / 3), species(4.0 * 2 ** (2 / 3), C / 3),
                    species(4.0 * 3 ** (2 / 3), C / 3)],
        atomic=True, reaction=None, note="rank-3 feasibility",
    )
    ms["M2+AGG"] = dict(
        components=[species(4.0, C * 0.495), species(4.4, C * 0.495),
                    species(20.0, C * 0.01)],
        atomic=True, reaction=None,
        note="M2-10 plus 1% w/w 20 S aggregate; high-moment contamination",
        support=(3.8, 4.6),
    )
    ms["BROAD"] = dict(
        components=_lognormal_components(4.0, 0.08, C),
        atomic=False, reaction=None,
        note="single log-normal peak, 8% width; non-atomic failure mode",
        support=(4.0 * np.exp(-2 * 0.08), 4.0 * np.exp(2 * 0.08)),
    )
    ms["RA-fast"] = dict(
        components=[species(4.0, C), species(4.0 * 2 ** (2 / 3), 0.0)],
        atomic=False,
        reaction=dict(kind="monomer-dimer", K=4.0),
        note="rapid monomer-dimer equilibrium; reaction boundary",
        # a reaction boundary spans monomer to dimer; the window has to cover
        # the whole range, which is the point of the test
        support=(4.0 * 0.95, 4.0 * 2 ** (2 / 3) * 1.05),
    )
    return ms


def support_bounds(model, pad=0.25):
    """
    [s_min, s_max] in Svedbergs, as known a priori from the experiment.

    A model may carry an explicit "support" key.  That is used for two
    distinct reasons:

      * broad / reacting models, where the min-max over the discretising
        components wildly overstates where the mass actually is;
      * contamination models, where the window is deliberately placed over
        the main peak only, and the question being asked is how much the
        out-of-window aggregate still leaks into the moments (spec D.4).
    """
    if model.get("support") is not None:
        return tuple(model["support"])
    ss = [c["s"] / SVEDBERG for c in model["components"] if c["c"] > 0]
    lo, hi = min(ss), max(ss)
    span = max(hi - lo, 0.5)
    return lo - pad * span, hi + pad * span


# --------------------------------------------------------------------------
# Simulation, including the rapid-equilibrium reacting case
# --------------------------------------------------------------------------

def initial_condition(grid, geom, signal_concentration):
    """
    Starting profile for one species.

    Sedimentation velocity: the cell is filled uniformly.
    Band forming: a lamella layered at the meniscus, with the shape and
    width US_Astfem_RSA uses (see forward/band.py).
    """
    if not getattr(geom, "band", False):
        return signal_concentration
    from band import lamella_profile
    return signal_concentration * lamella_profile(
        grid.r, geom.meniscus, band_volume=geom.band_volume)


def simulate(model, geom, n_cells=6000):
    """
    Simulate a model and return readings on the measured radial grid,
    shape (n_scans, n_r).
    """
    grid = LammGrid(geom.meniscus, geom.bottom, n_cells)
    if model["reaction"] is None:
        c = np.zeros((len(geom.times), grid.n))
        for comp in model["components"]:
            c += solve_species(grid, comp["s"], comp["D"], geom.rpm, geom.times,
                               c0=initial_condition(grid, geom, comp["c"]))
    else:
        c = _simulate_rapid_monomer_dimer(grid, model, geom)
    # interpolate solver cells onto the measured radial grid
    out = np.empty((len(geom.times), geom.r.size))
    for i in range(c.shape[0]):
        out[i] = np.interp(geom.r, grid.r, c[i])
    return out


def _equilibrate(c_tot, K):
    """
    Rapid monomer-dimer re-equilibration at each radius.

    c_tot = c_M + c_D with c_D = K c_M^2  =>  c_M = (-1 + sqrt(1+4 K c_tot))/(2K)
    """
    if K <= 0:
        return c_tot, np.zeros_like(c_tot)
    cm = (-1.0 + np.sqrt(1.0 + 4.0 * K * np.maximum(c_tot, 0.0))) / (2.0 * K)
    return cm, c_tot - cm


def _simulate_rapid_monomer_dimer(grid, model, geom, sub_per_scan=8):
    """
    Operator-split transport / reaction for a rapidly equilibrating
    monomer-dimer system.  Each sub-step transports the two species
    independently, then re-imposes local chemical equilibrium.  This is the
    correct fast-kinetics limit: the reaction boundary it produces is not a
    sum of Diracs, which is exactly what Gate G4 is probing.
    """
    from lamm import solve_species

    mono, dimer = model["components"]
    K = model["reaction"]["K"]
    tot0 = initial_condition(grid, geom, mono["c"] + dimer["c"])
    tot0 = np.full(grid.n, tot0) if np.ndim(tot0) == 0 else tot0
    cM, cD = _equilibrate(tot0, K)

    times = geom.times
    out = np.zeros((times.size, grid.n))
    t_prev = 0.0
    edges = np.concatenate([[0.0], times])
    for k in range(times.size):
        span = edges[k + 1] - edges[k]
        n_sub = max(1, sub_per_scan)
        dt = span / n_sub
        for _ in range(n_sub):
            cM = solve_species(grid, mono["s"], mono["D"], geom.rpm,
                               np.array([dt]), c0=cM)[0]
            cD = solve_species(grid, dimer["s"], dimer["D"], geom.rpm,
                               np.array([dt]), c0=cD)[0]
            cM, cD = _equilibrate(cM + cD, K)
        out[k] = cM + cD
        t_prev = times[k]
    return out


# --------------------------------------------------------------------------
# Feasible test set
# --------------------------------------------------------------------------
# The spec's test set is anchored at 4 S.  Phase B.3 shows that at 4 S the
# observable window is empty: the boundary never gets k*sigma clear of the
# meniscus before it reaches the bottom (see FINDINGS.md, B.3).  The set below
# is the same experimental design translated to s0 = 15 S, where an admissible
# window does exist, so that Phases C-E have data to run on.  Both sets are
# shipped: the 4 S set documents the limitation, the 15 S set produces the
# handoff numbers.

S0_FEASIBLE = 15.0


def feasible_test_set(s0=S0_FEASIBLE, rel=None):
    C = 0.5
    ms = {}
    ms["F-S1"] = dict(components=[species(s0, C)], atomic=True, reaction=None,
                      note=f"single species at {s0} S")
    for tag, ds in (("15", 0.15), ("10", 0.10), ("05", 0.05), ("03", 0.03)):
        ms[f"F-M2-{tag}"] = dict(
            components=[species(s0, C / 2), species(s0 * (1 + ds), C / 2)],
            atomic=True, reaction=None,
            note=f"two species, delta-s/s = {ds:.0%}")
    ms["F-M3"] = dict(
        components=[species(s0, C / 3), species(s0 * 1.06, C / 3),
                    species(s0 * 1.12, C / 3)],
        atomic=True, reaction=None, note="rank 3, 6% spacing")
    ms["F-M2+AGG"] = dict(
        components=[species(s0, C * 0.495), species(s0 * 1.10, C * 0.495),
                    species(s0 * 2.5, C * 0.01)],
        atomic=True, reaction=None,
        note="F-M2-10 plus 1% w/w aggregate at 2.5x s0",
        support=(s0 * 0.95, s0 * 1.15))
    ms["F-BROAD"] = dict(
        components=_lognormal_components(s0, 0.08, C), atomic=False,
        reaction=None, note="log-normal peak, 8% width",
        support=(s0 * np.exp(-2 * 0.08), s0 * np.exp(2 * 0.08)))
    ms["F-RA-fast"] = dict(
        components=[species(s0, C), species(s0 * 2 ** (2 / 3), 0.0)],
        atomic=False, reaction=dict(kind="monomer-dimer", K=4.0),
        note="rapid monomer-dimer equilibrium",
        support=(s0 * 0.95, s0 * 2 ** (2 / 3) * 1.05))
    return ms


def band_run_geometry(rpm=40000, n_scans=100, t_start=300.0, t_end=6000.0,
                      band_volume=0.015, **kw):
    """
    Run geometry for a band-forming experiment.

    The schedule is deliberately NOT the SV one.  A band pellets as soon as
    it reaches the bottom, and every scan after that is useless -- both for
    moments and for the band-gated noise estimate, whose whole premise is
    that most radii are empty.  Scheduling to the band's transit time rather
    than to a fixed 22000 s turns 15 usable scans into ~50 and cuts the
    residual TI noise by an order of magnitude.
    """
    return RunGeometry(rpm=rpm, n_scans=n_scans, t_start=t_start, t_end=t_end,
                       band=True, band_volume=band_volume, **kw)
