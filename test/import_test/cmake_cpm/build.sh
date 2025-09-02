#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

# get CPM.cmake
wget -O CPM.cmake https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake

rm -rf build

if [ -n "$GITHUB_ACTIONS" ]; then
  # These commands are for CI only. Please ignore them.
  cmake -S . -B build -G "Ninja Multi-Config" -DCPM_ex_actor_SOURCE="$GITHUB_WORKSPACE"
  cmake --build build --config Release
else
  # Please use the following commands in your project.
  cmake -S . -B build -G "Ninja Multi-Config"
  cmake --build build --config Release
fi
