#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC"/.. || exit

set -e -x

rm -rf build compile_commands.json
export CPM_SOURCE_CACHE=$HOME/.cache/CPM
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=1 "$@"
cp build/compile_commands.json ./

# remove args that are not supported by clangd
sed -i 's/-fconcepts-diagnostics-depth=[0-9]*//g' compile_commands.json
