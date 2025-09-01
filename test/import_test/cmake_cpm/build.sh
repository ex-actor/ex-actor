#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit

set -e -x

export CPM_SOURCE_CACHE=$HOME/.cache/CPM
rm -rf build
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Release