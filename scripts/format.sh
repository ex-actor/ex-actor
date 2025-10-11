#! /bin/bash
SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC"/.. || exit
set -e -x

git fetch origin main
git-clang-format-20 --binary "$(which clang-format-20)" -f origin/main || true
buildifier -r ./
