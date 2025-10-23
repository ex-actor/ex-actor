#! /bin/bash
SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

uvx --with "mkdocs-d2-plugin,mkdocs-material[imaging],mkdocs-add-number-plugin,mkdocs-enumerate-headings-plugin,mkdocs-git-revision-date-localized-plugin,mkdocs-git-committers-plugin-2" \
mkdocs build
