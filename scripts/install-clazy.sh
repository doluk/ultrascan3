#!/usr/bin/env bash
# Builds clazy (clazy-standalone) from source against Ubuntu's LLVM, because the distribution package is old
# (Ubuntu 24.04: clazy 1.11 on LLVM 15). The install prefix is meant to be cached between CI runs.
#
# usage: scripts/install-clazy.sh <clazy-tag> <llvm-major> <prefix>      e.g. v1.17.1 20 /opt/clazy
# Needs root (apt). Requires clazy >= 1.16 for LLVM >= 19.
set -euo pipefail
tag=${1:?clazy tag, e.g. v1.17.1}
llvm=${2:?llvm major version, e.g. 20}
prefix=${3:?install prefix}

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq

if [ -x "$prefix/bin/clazy-standalone" ]; then
   echo "clazy $tag found in $prefix (cache hit): installing the LLVM runtime only"
   apt-get install -y -qq "libclang-cpp$llvm" "libllvm$llvm" "libclang-common-$llvm-dev"
else
   echo "building clazy $tag against LLVM $llvm"
   apt-get install -y -qq git cmake ninja-build g++ "llvm-$llvm-dev" "libclang-$llvm-dev" "clang-$llvm" "libclang-cpp$llvm-dev"
   src=$(mktemp -d)
   git clone -q --depth 1 --branch "$tag" https://github.com/KDE/clazy.git "$src/clazy"
   cmake -S "$src/clazy" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
         -DCLAZY_BUILD_CLANG_TIDY=OFF -DCLAZY_BUILD_TESTS=OFF -DLLVM_CONFIG_EXECUTABLE="/usr/lib/llvm-$llvm/bin/llvm-config"
   cmake --build "$src/build" -j"$(nproc)"
   cmake --install "$src/build"
   rm -rf "$src"
fi
# no pipe into head: with pipefail a closed pipe (SIGPIPE) would fail the script
version=$("$prefix/bin/clazy-standalone" --version 2>&1 || true)
echo "$version" | sed -n 1,2p
