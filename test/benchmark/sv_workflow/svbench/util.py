"""Small helpers:  logged subprocess calls and the AUC binary format."""

import struct
import subprocess


def run_logged(cmd, log, env=None, cwd=None, timeout=None, setup=None):
    """Run cmd, appending its output to the file log; raise on failure.

    setup:  optional shell commands run first in a login bash, e.g.
    "module purge; module load ultrascan/gui", so that cmd runs in the
    environment of an environment-modules setup.
    """
    if setup:
        cmd = ["bash", "-lc", setup + '\nexec "$@"', "svbench"] + list(cmd)
    with open(log, "a") as fh:
        fh.write("\n$ " + " ".join(cmd) + "\n")
        fh.flush()
        try:
            proc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT,
                                  env=env, cwd=cwd, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise RuntimeError("command timed out after %g s (a GUI dialog "
                               "waiting for input?), see %s: %s"
                               % (timeout, log, " ".join(cmd)))
    if proc.returncode != 0:
        raise RuntimeError("command failed (%d), see %s: %s"
                           % (proc.returncode, log, " ".join(cmd)))


def read_auc(path):
    """Read an UltraScan AUC file (US_DataIO::writeRawData layout).

    Returns (radii, times, values) where values has shape (scans, points).
    Readings are stored as 16-bit integers scaled to [min, max] of the
    data, so they carry a quantization error of (max - min) / 65535.
    """
    import numpy as np

    with open(path, "rb") as fh:
        buf = fh.read()
    if buf[:4] != b"UCDA":
        raise ValueError("not an AUC file: " + path)
    pos = 4 + 2 + 2 + 1 + 1 + 16 + 240
    min_r, max_r, d_r, min_d, max_d, _, _ = struct.unpack_from("<7f", buf,
                                                                pos)
    pos += 28
    (nscan,) = struct.unpack_from("<H", buf, pos)
    pos += 2
    delta = (max_d - min_d) / 65535.0
    times = []
    rows = []
    for _ in range(nscan):
        if buf[pos:pos + 4] != b"DATA":
            raise ValueError("bad scan marker in " + path)
        _, _, secs, _ = struct.unpack_from("<ffIf", buf, pos + 4)
        (count,) = struct.unpack_from("<I", buf, pos + 26)
        pos += 30
        raw = np.frombuffer(buf, dtype="<u2", count=count, offset=pos)
        rows.append(min_d + raw.astype(float) * delta)
        pos += 2 * count + (count + 7) // 8
        times.append(float(secs))
    radii = min_r + d_r * np.arange(len(rows[0]))
    return radii, np.array(times), np.vstack(rows)
