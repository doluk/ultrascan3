"""Parse the driver's sweep-tag convention and group runs by mesh
configuration ("series") so refine/uniform variations plot as separate,
consistently colored lines instead of being averaged or overplotted
together.

Tag convention: ``<kind>_<value>[_<series>]`` where ``kind`` is ``N`` or
``dt``, ``value`` is numeric, and ``series`` is an arbitrary suffix such as
``R0_U1`` (refine=0, uniform=1). Tags without a recognized ``N_``/``dt_``
prefix (e.g. a one-off ``band_mesh`` run) are not part of a sweep and are
left out of grouping.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Sequence

from ._palette import CAT_SERIES_ORDER, MARKER_SERIES_ORDER
from .trace_io import TraceRun

_TAG_RE = re.compile(r"^(N|dt)_([0-9]+(?:\.[0-9]+)?)(?:_(.+))?$")

# Default label for runs whose tag has no series suffix (e.g. plain "N_100").
DEFAULT_SERIES = "default"


@dataclass
class TagInfo:
    kind: str | None      # "N", "dt", or None if the tag isn't a sweep point
    value: float | None
    series: str           # e.g. "R0_U1", or DEFAULT_SERIES if unsuffixed


def parse_tag(tag: str) -> TagInfo:
    m = _TAG_RE.match(tag)
    if not m:
        return TagInfo(kind=None, value=None, series=DEFAULT_SERIES)
    kind, value, series = m.groups()
    return TagInfo(kind=kind, value=float(value), series=series or DEFAULT_SERIES)


def series_label(series: str) -> str:
    """Human-readable label for a series suffix, e.g. 'R1_U0' ->
    'refine=1, uniform=0'. Falls back to the raw string otherwise."""
    m = re.match(r"^R([0-9]+)_U([0-9]+)$", series)
    if m:
        return f"refine={m.group(1)}, uniform={m.group(2)}"
    return series


def group_sweep(runs: Sequence[TraceRun], kind: str) -> dict:
    """Group runs of the given kind ('N' or 'dt') by series, sorted by
    sweep value within each series. Returns {series: [(value, run), ...]}.
    """
    groups: dict = {}
    for run in runs:
        info = parse_tag(run.tag)
        if info.kind != kind:
            continue
        groups.setdefault(info.series, []).append((info.value, run))
    for series in groups:
        groups[series].sort(key=lambda pair: pair[0])
    return groups


def series_style(series_names: Sequence[str]) -> dict:
    """Deterministic color+marker per series, in fixed categorical order
    (sorted series names -> palette slots in order; never reassigned based
    on data values)."""
    ordered = sorted(series_names)
    return {
        name: {
            "color": CAT_SERIES_ORDER[i % len(CAT_SERIES_ORDER)],
            "marker": MARKER_SERIES_ORDER[i % len(MARKER_SERIES_ORDER)],
        }
        for i, name in enumerate(ordered)
    }
