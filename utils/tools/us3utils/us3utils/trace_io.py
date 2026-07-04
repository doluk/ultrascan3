"""Load US_LammAstfvm solution-trace CSV pairs (trace_steps / trace_nodes)."""

from __future__ import annotations

import dataclasses
from pathlib import Path

import numpy as np
import pandas as pd


def _parse_meta_lines(path: Path) -> dict:
    """Parse the leading '# meta: key,val key2,val2 ...' comment lines."""
    meta: dict = {}
    with open(path, "r") as fh:
        for line in fh:
            if not line.startswith("#"):
                break
            body = line.split("# meta:", 1)[-1].strip()
            for token in body.split():
                if "," not in token:
                    continue
                key, _, val = token.partition(",")
                try:
                    fval = float(val)
                    meta[key] = int(fval) if fval.is_integer() and "." not in val else fval
                except ValueError:
                    meta[key] = val
    return meta


@dataclasses.dataclass
class TraceRun:
    """One (N, dt, ...) run: metadata + per-step scalars + per-node rows."""

    tag: str
    meta: dict
    steps: pd.DataFrame
    nodes: pd.DataFrame

    @property
    def N_init(self) -> int:
        return int(self.meta.get("N_init", 0))

    @property
    def dt(self) -> float:
        """Effective dt: first row's dt if fixed_dt not given."""
        fixed = float(self.meta.get("fixed_dt", 0.0))
        if fixed > 0.0:
            return fixed
        if len(self.steps):
            return float(self.steps["dt"].iloc[0])
        return 0.0

    def nodes_at_time(self, time: float, vertices_only: bool = True) -> pd.DataFrame:
        """Nodes from the step whose 'time' is closest to the requested time."""
        step_times = self.steps["time"].to_numpy()
        idx = (abs(step_times - time)).argmin()
        step_no = self.steps["step"].iloc[idx]
        sub = self.nodes[self.nodes["step"] == step_no]
        if vertices_only:
            sub = sub[sub["is_midpoint"] == 0]
        return sub.sort_values("r").reset_index(drop=True)

    def nodes_bracketing_time(self, time: float, vertices_only: bool = True):
        """The two recorded steps bracketing ``time`` (t0 <= time <= t1), for
        proper time interpolation instead of snap-to-nearest. Returns
        ``(t0, nodes0, t1, nodes1)``; t0 == t1 (nodes0 is nodes1) when
        ``time`` lands exactly on a recorded step or the run has only one.
        """
        step_times = self.steps["time"].to_numpy()
        idx1 = int(np.searchsorted(step_times, time))
        idx1 = min(max(idx1, 0), len(step_times) - 1)
        idx0 = idx1 if step_times[idx1] <= time else max(idx1 - 1, 0)
        idx1 = idx0 if step_times[idx0] >= time else min(idx0 + 1, len(step_times) - 1)
        t0, t1 = float(step_times[idx0]), float(step_times[idx1])

        def _nodes_for(idx):
            step_no = self.steps["step"].iloc[idx]
            sub = self.nodes[self.nodes["step"] == step_no]
            if vertices_only:
                sub = sub[sub["is_midpoint"] == 0]
            return sub.sort_values("r").reset_index(drop=True)

        return t0, _nodes_for(idx0), t1, _nodes_for(idx1)


def load_trace(steps_csv: str | Path, nodes_csv: str | Path | None = None,
               tag: str | None = None) -> TraceRun:
    """Load a trace pair.

    ``steps_csv`` may be given as either the '*_trace_steps.csv' path, or the
    common '<dir>/<tag>_c<comp>' base (in which case both suffixes are
    resolved automatically).
    """
    steps_csv = Path(steps_csv)
    if nodes_csv is None:
        if steps_csv.name.endswith("_trace_steps.csv"):
            base = steps_csv.name[: -len("_trace_steps.csv")]
            nodes_csv = steps_csv.with_name(base + "_trace_nodes.csv")
        else:
            nodes_csv = steps_csv.with_name(steps_csv.stem + "_trace_nodes.csv")
    nodes_csv = Path(nodes_csv)

    meta = _parse_meta_lines(steps_csv)
    steps = pd.read_csv(steps_csv, comment="#")
    nodes = pd.read_csv(nodes_csv)

    if tag is None:
        tag = meta.get("tag", steps_csv.stem)

    return TraceRun(tag=tag, meta=meta, steps=steps, nodes=nodes)


def load_sweep(directory: str | Path, pattern: str = "*_trace_steps.csv") -> list[TraceRun]:
    """Load every trace pair in a directory matching the glob pattern."""
    directory = Path(directory)
    runs = []
    for steps_csv in sorted(directory.glob(pattern)):
        runs.append(load_trace(steps_csv))
    return runs
