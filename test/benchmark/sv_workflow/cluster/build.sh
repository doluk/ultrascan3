#!/usr/bin/env bash
# Build the three executables the benchmark needs with qmake (Qt5):
#   us_mpi_analysis of main (old NNLS) and of the branch (new NNLS), and
#   us_astfem_sim of the branch (it has --seed/--odlimit), used for all
#   simulations.
#
# USAGE
#   cluster/build.sh DEST LOCAL_PRI_HPC LOCAL_PRI_GUI [MAIN_REV] [BRANCH_REV]
#
#   DEST           directory for the worktrees, builds and wrappers
#   LOCAL_PRI_HPC  local.pri of the cluster's us_mpi_analysis build
#                  (NO_DB, MPIPATH/MPILIBS, ...)
#   LOCAL_PRI_GUI  local.pri of a desktop (GUI) build:  qwt, mysql, X11
#   MAIN_REV       revision with the old NNLS (default 5a137ef)
#   BRANCH_REV     revision with the new NNLS (default HEAD)
#
# Environment:
#   QMAKE            qmake to use (default: qmake), e.g. the qmake of the
#                    Qt installation in QWTPATH
#   MAKE_JOBS        parallel make jobs (default: nproc)
#   WRAPPER_LD_PATH  extra run-time library dirs for the wrappers;
#                    QWTPATH/lib and MPIPATH/lib of the .pri files are
#                    added automatically when they exist
#
# Every build has its own lib/ (both contain a libus_utils), so the
# executables are run through wrappers in DEST/bin that set
# LD_LIBRARY_PATH;  use those wrapper paths in config.json (printed at
# the end).
set -euo pipefail

DEST=$(realpath -m "${1:?destination directory}")
PRI_HPC=$(realpath "${2:?local.pri for the HPC build}")
PRI_GUI=$(realpath "${3:?local.pri for the GUI build}")
MAIN_REV=${4:-5a137ef}
BRANCH_REV=${5:-HEAD}
QMAKE=${QMAKE:-qmake}
JOBS=${MAKE_JOBS:-$(nproc)}
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BRANCH_REV=$(git -C "$REPO" rev-parse "$BRANCH_REV")

"$QMAKE" --version | grep -q "Qt version 5" \
  || { echo "ERROR: $QMAKE is not a Qt5 qmake" >&2; exit 1; }

mkdir -p "$DEST/bin"

worktree() {   # worktree DIR REV
  if [ ! -d "$1" ]; then
    git -C "$REPO" worktree add --detach "$1" "$2"
  fi
}

qbuild() {     # qbuild DIR PRO
  ( cd "$1" && "$QMAKE" "$2" && make -j "$JOBS" )
}

prival() {     # prival FILE VAR:  first uncommented "VAR = value" of a .pri
  sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*/\1/p" \
      "$1" | head -n 1
}

libpath() {    # libpath PRI:  run-time library dirs (Qwt/Qt, MPI) of a build
  local out="" v
  for v in QWTPATH MPIPATH; do
    v=$(prival "$1" "$v")
    if [ -n "$v" ] && [ -d "$v/lib" ]; then out="$out:$v/lib"; fi
  done
  echo "${WRAPPER_LD_PATH:+:$WRAPPER_LD_PATH}$out"
}

wrapper() {    # wrapper NAME TREE EXE PRI
  local lp
  lp="$2/lib$(libpath "$4")"
  {
    echo '#!/bin/sh'
    echo "export LD_LIBRARY_PATH=\"$lp\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\""
    echo "exec \"$3\" \"\$@\""
  } > "$DEST/bin/$1"
  chmod +x "$DEST/bin/$1"
}

# us_mpi_analysis:  utils library + program, per revision
for name in main branch; do
  rev=$MAIN_REV; [ "$name" = branch ] && rev=$BRANCH_REV
  src="$DEST/src-$name-hpc"
  worktree "$src" "$rev"
  cp "$PRI_HPC" "$src/local.pri"
  qbuild "$src/utils" libus_utils.pro
  qbuild "$src/programs/us_mpi_analysis" us_mpi_analysis.pro
  wrapper "us_mpi_analysis_$name" "$src" \
          "$src/programs/us_mpi_analysis/us_mpi_analysis" "$PRI_HPC"
done

# us_astfem_sim:  qwtplot3d, utils, gui libraries + program (branch)
src="$DEST/src-branch-gui"
worktree "$src" "$BRANCH_REV"
cp "$PRI_GUI" "$src/local.pri"
qbuild "$src/qwtplot3d" qwtplot3d.pro
qbuild "$src/utils" libus_utils.pro
qbuild "$src/gui" libus_gui.pro
qbuild "$src/programs/us_astfem_sim" us_astfem_sim.pro
wrapper us_astfem_sim "$src" "$src/bin/us_astfem_sim" "$PRI_GUI"

cat <<EOF

Built.  Executable entries for config.json:
  "astfem_sim": "$DEST/bin/us_astfem_sim",
  "mpi_analysis_main": "$DEST/bin/us_mpi_analysis_main",
  "mpi_analysis_branch": "$DEST/bin/us_mpi_analysis_branch",
EOF
