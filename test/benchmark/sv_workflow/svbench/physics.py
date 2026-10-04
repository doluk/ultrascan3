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


def scan_schedule(solutes, rpm, meniscus, right_edge, sim):
    """Times (s) of the first and last scan for one run.

    The last scan is taken when the boundary of the slowest solute has
    crossed `last_scan_fraction` of the data range, the first when the
    boundary of the fastest solute has crossed `first_scan_fraction` of it.
    Times are clipped to [acceleration + first_scan_min_delay,
    max_run_hours] and the scans are kept at least `min_scan_interval`
    seconds apart, so slow solutes at low speed yield diffusion-dominated
    data and fast solutes at high speed may pellet before the first scan,
    as in a real experiment with a fixed scan rate.
    """
    span = right_edge - meniscus
    s_lo = min(x["s"] for x in solutes)
    s_hi = max(x["s"] for x in solutes)
    accel_time = rpm / float(sim["acceleration"])
    t_min = accel_time + sim["first_scan_min_delay"]
    t_max = sim["max_run_hours"] * 3600.0

    t_last = boundary_time(s_lo, rpm, meniscus,
                           meniscus + sim["last_scan_fraction"] * span)
    t_first = boundary_time(s_hi, rpm, meniscus,
                            meniscus + sim["first_scan_fraction"] * span)
    t_first = min(max(t_first, t_min), t_max)
    t_last = min(t_last, t_max)
    t_last = max(t_last, t_first + (sim["scans"] - 1)
                 * sim["min_scan_interval"])
    return round(t_first), round(t_last)
