#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit

set -e -x

rm -rf build
export CPM_SOURCE_CACHE=$HOME/.cache/CPM
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=1
