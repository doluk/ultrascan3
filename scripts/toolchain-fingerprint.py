#!/usr/bin/env python3
"""Fingerprint the inputs that determine each prebuilt toolchain target.

A toolchain target only needs rebuilding when something that ends up inside it
changes. This computes, per target, a digest over exactly those files, so the
toolchain workflow can compare it against the digest recorded in
buildsys/toolchain.lock.json and skip targets whose inputs are unchanged.

Run it by hand to find out whether a change will cost a toolchain build:

    python3 scripts/toolchain-fingerprint.py

The digest covers git-tracked content only, so an untracked build artifact
sitting in a source tree cannot make a local run disagree with CI.

buildsys/toolchain.lock.json is deliberately NOT an input, even though the
Dockerfiles copy it into the image: a toolchain run rewrites that file, so
including it would make every pin update invalidate the toolchain it just
pinned, and the workflow would rebuild forever.
"""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# The dependency manifest, the ports that build it, and the triplet and
# toolchain files that decide how every package is compiled. Directories are
# taken whole; anything tracked below them counts.
COMMON = [
    "vcpkg.json",
    "vcpkg-configuration.json",
    "buildsys/vcpkg/overlay-ports",
    "qwtplot3d",
    "admin/cmake/triplets",
    "admin/cmake/toolchain.cmake",
]

# Per target, the driver that runs vcpkg and the bootstrap that installs the
# compiler it runs with. Kept separate so a macOS bootstrap edit cannot cost a
# Windows rebuild, and a Dockerfile edit cannot cost either.
#
# Keep this in sync with the push-paths filter in
# .github/workflows/toolchain-build.yml: a run has to start before it can
# decide anything, so every input here must also be a path that triggers it.
TARGETS = {
    "macos-arm64": COMMON + [
        "scripts/build-toolchain.sh",
        "scripts/bootstrap-macos.sh",
    ],
    "macos-x64": COMMON + [
        "scripts/build-toolchain.sh",
        "scripts/bootstrap-macos.sh",
    ],
    "windows-x64": COMMON + [
        "scripts/build-toolchain.ps1",
        "scripts/bootstrap-windows.ps1",
    ],
    "ubuntu2404-x64": COMMON + [
        "scripts/build-toolchain.sh",
        "scripts/bootstrap-linux.sh",
        "buildsys/docker/ubuntu2404.Dockerfile",
    ],
    "oraclelinux8-x64": COMMON + [
        "scripts/build-toolchain.sh",
        "scripts/bootstrap-linux.sh",
        "buildsys/docker/oraclelinux8.Dockerfile",
    ],
}


def tracked_files(paths):
    """Return the tracked files under `paths`, sorted, as repo-relative strings.

    An input that matches nothing is an error rather than an empty
    contribution: a renamed or deleted source would otherwise silently drop out
    of the fingerprint and freeze the target at whatever it was last built from.
    """
    out = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-files", "-s", "-z", "--"] + list(paths),
        check=True, capture_output=True, text=True,
    ).stdout

    found = {}
    for record in out.split("\0"):
        if not record:
            continue
        # "<mode> <blob sha> <stage>\t<path>"
        meta, path = record.split("\t", 1)
        found[path] = meta.split()[0]

    for wanted in paths:
        if not any(p == wanted or p.startswith(wanted + "/") for p in found):
            raise SystemExit(
                f"toolchain-fingerprint: '{wanted}' matches no tracked file.\n"
                f"Update TARGETS in {__file__} to match the repository layout."
            )

    return sorted(found.items())


def fingerprint(paths):
    """Digest the content and mode of every tracked file under `paths`."""
    digest = hashlib.sha256()
    for path, mode in tracked_files(paths):
        blob = (REPO_ROOT / path).read_bytes()
        digest.update(f"{mode} {path}\0".encode())
        digest.update(hashlib.sha256(blob).digest())
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=sorted(TARGETS),
                        help="print one target's digest and nothing else")
    parser.add_argument("--json", action="store_true",
                        help="print every target as a JSON object")
    args = parser.parse_args()

    if args.target:
        print(fingerprint(TARGETS[args.target]))
        return

    digests = {t: fingerprint(p) for t, p in TARGETS.items()}

    if args.json:
        print(json.dumps(digests))
        return

    lock = json.loads((REPO_ROOT / "buildsys/toolchain.lock.json").read_text())
    width = max(len(t) for t in digests)
    for target, current in sorted(digests.items()):
        recorded = (lock["targets"].get(target) or {}).get("inputs_digest")
        if not recorded:
            state = "never built here"
        elif recorded == current:
            state = "up to date"
        else:
            state = "REBUILD NEEDED"
        print(f"{target:<{width}}  {current[:16]}  {state}")


if __name__ == "__main__":
    sys.exit(main())
