#! /bin/bash
SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

uvx --with mkdocs-d2-plugin,mkdocs-material  mkdocs serve -a 0.0.0.0:8000