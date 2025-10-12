#! /bin/bash
SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

uvx --with mkdocs-d2-plugin,mkdocs-material,mkdocs-add-number-plugin,mkdocs-enumerate-headings-plugin  mkdocs serve -a 0.0.0.0:2023
