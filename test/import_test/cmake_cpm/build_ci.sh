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

export CPM_SOURCE_CACHE=$HOME/.cache/CPM

cmake -S . -B build -G "Ninja Multi-Config" -DCPM_ex_actor_SOURCE="$GITHUB_WORKSPACE"
cmake --build build --config Release