"""
Realistic noise injection.

The point of this module is that additive white noise alone is the easy case.
For interference optics the systematic components dominate the random one by
one to two orders of magnitude:

    random (white)          0.005 - 0.01 fringes RMS
    time-invariant  (TI)    0.1   - 0.5  fringes      <- function of r only
    radially-invariant (RI) 0.05  - 0.1  fringes/scan <- function of t only

TI noise is the optical signature of the cell windows and is fixed for the
run.  RI noise is a per-scan piston, mostly from fringe-count jitter.

Real TI/RI vectors harvested from an instrument (US_Noise .xml files) are
strongly preferred over the synthetic ones here; `load_us_noise` reads them.
The synthetic TI vector is built to have the right character -- smooth,
large-scale window distortion plus a few sharp scratches -- rather than being
white, because a white "TI" vector would be far easier to reject than the
real thing.
"""

import numpy as np

PRESETS = {
    "interference": dict(white=0.008, ti=0.25, ri=0.07),
    "absorbance":   dict(white=0.008, ti=0.02, ri=0.0),
    "white-only":   dict(white=0.008, ti=0.0, ri=0.0),
}


def synth_ti(r, amplitude, rng, n_smooth=6, n_scratch=3):
    """
    Synthetic time-invariant vector over the radial grid.

    Smooth low-order window distortion (the bulk of real TI) plus a few
    narrow scratch-like features.
    """
    x = (r - r[0]) / (r[-1] - r[0])
    v = np.zeros_like(x)
    for k in range(1, n_smooth + 1):
        v += rng.normal() / k * np.sin(np.pi * k * x + rng.uniform(0, np.pi))
    for _ in range(n_scratch):
        c = rng.uniform(0.05, 0.95)
        w = rng.uniform(0.002, 0.01)
        v += rng.normal() * 0.35 * np.exp(-0.5 * ((x - c) / w) ** 2)
    v -= v.mean()
    rms = np.sqrt((v ** 2).mean())
    return v * (amplitude / rms) if rms > 0 else v


def synth_ri(n_scans, amplitude, rng, drift=0.5):
    """
    Synthetic radially-invariant vector: one offset per scan, part random
    walk (slow drift in fringe count) and part independent jitter.
    """
    walk = np.cumsum(rng.normal(size=n_scans))
    walk -= walk.mean()
    if walk.std() > 0:
        walk /= walk.std()
    jit = rng.normal(size=n_scans)
    v = drift * walk + (1 - drift) * jit
    v -= v.mean()
    rms = np.sqrt((v ** 2).mean())
    return v * (amplitude / rms) if rms > 0 else v


class NoiseGenerator:
    """
    Draws noise realizations for a fixed experiment.

    The TI vector is drawn ONCE per experiment (it is a property of the cell,
    not of the scan) and re-used across Monte Carlo realizations only if
    `fixed_ti` is set.  For the covariance study we redraw it each time, so
    that Sigma reflects our ignorance of the actual TI vector.
    """

    def __init__(self, r, n_scans, preset="interference", seed=0, fixed_ti=False):
        self.r = np.asarray(r, float)
        self.n_scans = int(n_scans)
        self.cfg = dict(PRESETS[preset])
        self.preset = preset
        self.rng = np.random.default_rng(seed)
        self.fixed_ti = fixed_ti
        self._ti = synth_ti(self.r, self.cfg["ti"], self.rng) if fixed_ti and self.cfg["ti"] > 0 else None

    def draw(self, clean, return_parts=False):
        """
        Return a noisy copy of `clean` (n_scans, n_r).

        With return_parts, also return the TI vector actually drawn, so that
        an 'oracle' TI-removal bound can be computed (Phase D).
        """
        out = np.array(clean, dtype=float, copy=True)
        if self.cfg["white"] > 0:
            out += self.rng.normal(0.0, self.cfg["white"], size=out.shape)
        ti = np.zeros(self.r.size)
        if self.cfg["ti"] > 0:
            ti = self._ti if self._ti is not None else synth_ti(self.r, self.cfg["ti"], self.rng)
            out += ti[None, :]
        if self.cfg["ri"] > 0:
            out += synth_ri(self.n_scans, self.cfg["ri"], self.rng)[:, None]
        return (out, ti) if return_parts else out


def load_us_noise(path):
    """
    Read an UltraScan US_Noise XML file and return (kind, values).

    kind is 'ti' or 'ri'.  Use this to inject noise vectors harvested from
    real runs instead of the synthetic ones above -- it costs an afternoon and
    makes the whole study far more defensible.
    """
    import xml.etree.ElementTree as ET

    root = ET.parse(path).getroot()
    node = root.find(".//noise")
    if node is None:
        raise ValueError(f"{path}: no <noise> element")
    kind = node.get("type", "ti").lower()
    vals = [float(v.get("v")) for v in node.findall("./value")]
    return ("ri" if kind.startswith("ri") else "ti"), np.asarray(vals)
