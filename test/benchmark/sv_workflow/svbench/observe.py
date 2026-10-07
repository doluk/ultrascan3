"""Which solute parameters a simulated experiment can determine at all.

Computed from the simulated truth only (scan times, data range, s and D
of each solute), never from the fits, so it treats all arms alike.

  displacement    distance the sedimentation boundary moves between the
                  first and last scan, as a fraction of the data range.
                  Below MIN_DISPLACEMENT the solute shows (almost) no
                  sedimentation in the data:  s and its concentration are
                  not determined (its signal is a nearly flat, time-
                  invariant plateau that TI/RI noise can absorb).
  diffusion_width sqrt(2 D t_last) as a fraction of the data range.
                  Below MIN_DIFFUSION_WIDTH the boundary hardly spreads:
                  s is still determined, but D, f/f0 and MW are not.

The thresholds follow the benchmark itself:  errors are large in every
bin below them and normal above (cluster64 interim results:  median |s|
and |conc| errors 6-15 % and 30-94 % below a displacement of 0.15, 0.2-1 %
and 1-3 % above 0.15; median |f/f0| errors 5-31 % below a diffusion width
of 0.04, 1-3 % above 0.05).
"""

import math

MIN_DISPLACEMENT = 0.2
MIN_DIFFUSION_WIDTH = 0.05


def solutes(truth, rpm, acceleration=400.0):
    """Observability of each true solute of a run (list of dicts)."""
    w2 = (rpm * math.pi / 30.0) ** 2
    t_acc = rpm / float(acceleration)
    m = truth["meniscus"]
    left, right = truth["data_left"], truth["data_right"]
    span = right - left
    t1, t2 = truth["time_first"], truth["time_last"]

    def boundary(s_sv, t):
        # omega^2 t of a linear acceleration ramp is w2 * (t - 2/3 t_acc)
        return m * math.exp(s_sv * 1e-13 * w2 * max(t - 2.0 * t_acc / 3.0,
                                                     0.0))

    out = []
    for sol in truth["solutes"]:
        r1, r2 = boundary(sol["s"], t1), boundary(sol["s"], t2)
        disp = (min(r2, right) - max(r1, left)) / span if r2 > left else 0.0
        width = math.sqrt(2.0 * sol["D"] * t2) / span
        sed_ok = disp >= MIN_DISPLACEMENT
        out.append({"displacement": max(disp, 0.0),
                    "diffusion_width": width,
                    "boundary_first": (r1 - left) / span,
                    "boundary_last": (r2 - left) / span,
                    "sed_ok": sed_ok,
                    "diff_ok": sed_ok and width >= MIN_DIFFUSION_WIDTH})
    return out
