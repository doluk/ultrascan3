"""Hydrodynamic relations (as in US_Model::calc_coefficients) and the
scan schedule of a simulated run."""

import math

AVOGADRO = 6.022140857e+23
R_GC = 8.314472e+07
K20 = 293.15
VISC_20W = 1.001600
DENS_20W = 0.998213
RT = R_GC * K20


def coefficients(s_sv, ff0, vbar):
    """D (cm^2/s), f, f0 and molecular weight (Da) from s (S), f/f0, vbar,
    following US_Model::calc_coefficients for the (s, f/f0) case."""
    s = s_sv * 1.0e-13
    buoy = 1.0 - vbar * DENS_20W
    numer = 0.02 * vbar * s * VISC_20W * ff0
    f0 = 0.09 * VISC_20W * math.pi * math.sqrt(numer / buoy)
    f = ff0 * f0
    D = RT / (AVOGADRO * f)
    mw = s * RT / (D * buoy)
    return {"s": s, "D": D, "f": f, "f0": f0, "mw": mw, "ff0": ff0,
            "vbar": vbar}


def omega2(rpm):
    return (rpm * math.pi / 30.0) ** 2


def boundary_time(s_sv, rpm, meniscus, radius):
    """Time (s) for a boundary of s (S) to move from meniscus to radius."""
    return math.log(radius / meniscus) / (s_sv * 1.0e-13 * omega2(rpm))


def scan_schedule(solutes, rpm, meniscus, bottom, sim):
    """Times (s) of the first and last scan for one run.

    The first scan is taken right after the rotor reaches speed:
    rpm / acceleration + `first_scan_delay` (1 s).  The last scan is taken
    when the boundary of the slowest solute has reached the bottom, times
    `last_scan_safety` (1.1) to let its diffusion-broadened boundary clear
    the data range:  t_last = safety * ln(bottom/meniscus) / (s_min w^2).
    t_last is capped at `max_run_hours` (very slow solutes at low speed)
    and kept at least (scans-1) * `min_scan_interval` after the first scan
    (very fast solutes at high speed).
    """
    s_lo = min(x["s"] for x in solutes)
    t_first = rpm / float(sim["acceleration"]) + sim["first_scan_delay"]
    t_last = sim["last_scan_safety"] * boundary_time(s_lo, rpm, meniscus,
                                                     bottom)
    t_last = min(t_last, sim["max_run_hours"] * 3600.0)
    t_last = max(t_last, t_first + (sim["scans"] - 1)
                 * sim["min_scan_interval"])
    return round(t_first), round(t_last)
