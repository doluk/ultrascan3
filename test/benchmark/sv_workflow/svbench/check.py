"""Pre-flight check of a node:  can the configured programs start here?

Each executable is started once, in the environment it gets during a
task (sim_setup / mpi_setup, private UltraScan settings), with arguments
that make it exit at once.  What matters is whether it starts at all:  a
missing program or shared library gives exit code 127 or the loader's
"error while loading shared libraries"; any other ending (e.g. the usage
error of us_mpi_analysis without a job) counts as a start.
"""

import os
import shlex
import shutil
import socket
import subprocess
import tempfile

LOADER_ERRORS = ("error while loading shared libraries", "not found",
                 "No such file or directory", "cannot execute")


def _run(cmd, setup, env, cwd, timeout=120):
    if setup:
        cmd = ["bash", "-lc", setup + '\nexec "$@"', "svbench"] + list(cmd)
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, env=env, cwd=cwd,
                           timeout=timeout)
        return p.returncode, p.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired:
        return None, "timed out after %d s (waiting for a dialog?)" % timeout
    except OSError as exc:
        return 127, str(exc)


def check(cfg):
    """Return a list of problems (empty if the node is usable)."""
    from .runner import us3_env
    problems = []
    root = tempfile.mkdtemp(prefix="svbench-check-",
                            dir=cfg["scratch"] if os.path.isdir(
                                cfg["scratch"]) else None)
    try:
        if not os.path.exists(cfg["us3_settings"]):
            problems.append("us3_settings not found: " + cfg["us3_settings"])
            return problems
        env = us3_env(cfg, root)

        rc, out = _run([cfg["astfem_sim"], "--help"], cfg["sim_setup"],
                       env, root)
        if rc is None or rc == 127 or any(e in out for e in LOADER_ERRORS):
            problems.append("us_astfem_sim does not start (exit %s):\n%s"
                            % (rc, out.strip()[-1500:]))

        launcher = shlex.split(cfg["mpirun"])[0]
        rc, out = _run(["bash", "-c", 'command -v "$0"', launcher],
                       cfg["mpi_setup"], dict(os.environ), root)
        if rc != 0:
            problems.append("MPI launcher not found: %s" % launcher)

        for key in ("mpi_analysis_main", "mpi_analysis_branch"):
            rc, out = _run([cfg[key]], cfg["mpi_setup"], dict(os.environ),
                           root, timeout=60)
            if rc == 127 or any(e in out for e in LOADER_ERRORS[:1]):
                problems.append("%s does not start (exit %s):\n%s"
                                % (key, rc, out.strip()[-1500:]))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return problems


def main(cfg):
    problems = check(cfg)
    host = socket.gethostname()
    if problems:
        print("node %s NOT usable:" % host)
        for p in problems:
            print("  - " + p.replace("\n", "\n    "))
        return 1
    print("node %s ok" % host)
    return 0
