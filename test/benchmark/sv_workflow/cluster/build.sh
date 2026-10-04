#!/usr/bin/env bash
# Build the three executables the benchmark needs:
#   us_mpi_analysis of main (old NNLS) and of the branch (new NNLS), and
#   us_astfem_sim (with --seed/--odlimit) used for all simulations.
#
# USAGE
#   cluster/build.sh DEST [MAIN_REV] [BRANCH_REV]
#
#   DEST        directory for worktrees and builds
#   MAIN_REV    revision of the old NNLS (default 5a137ef)
#   BRANCH_REV  revision of the new NNLS (default HEAD)
#
# Extra CMake arguments (toolchain file, Qt prefix, ...) can be passed in
# CMAKE_ARGS.  Prints a config.json fragment with the executable paths.
set -euo pipefail

DEST=$(realpath -m "${1:?destination directory}")
MAIN_REV=${2:-5a137ef}
BRANCH_REV=${3:-HEAD}
JOBS=${JOBS:-$(nproc)}
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
read -r -a XARGS <<< "${CMAKE_ARGS:-}"

mkdir -p "$DEST"
for name in main branch; do
  rev=$MAIN_REV; [ "$name" = branch ] && rev=$BRANCH_REV
  src="$DEST/src-$name"
  if [ ! -d "$src" ]; then
    git -C "$REPO" worktree add --detach "$src" "$rev"
  fi
  cmake -S "$src" -B "$DEST/build-hpc-$name" -G Ninja \
        -DUS3_PROFILE=HPC -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_DOCUMENTATION=OFF "${XARGS[@]}"
  ninja -C "$DEST/build-hpc-$name" -j "$JOBS" us_mpi_analysis
done

# The simulator comes from the branch (it carries the --seed and --odlimit
# options); the simulation code itself is the same in both revisions up to
# the closed-form fixed-mesh stiffness, which is numerically equivalent.
cmake -S "$DEST/src-branch" -B "$DEST/build-app" -G Ninja \
      -DUS3_PROFILE=APP -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_DOCUMENTATION=OFF "${XARGS[@]}"
ninja -C "$DEST/build-app" -j "$JOBS" us_astfem_sim

cat <<EOF
  "astfem_sim": "$DEST/build-app/bin/us_astfem_sim",
  "mpi_analysis_main": "$DEST/build-hpc-main/bin/us_mpi_analysis",
  "mpi_analysis_branch": "$DEST/build-hpc-branch/bin/us_mpi_analysis",
EOF
