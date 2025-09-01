#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit

set -e -x

rm -rf build
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_PREFIX_PATH=${HOME}/.cmake/packages/
cmake --build build --config Release