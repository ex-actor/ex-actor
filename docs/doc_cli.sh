#! /bin/bash
SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

rm -rf ./contents/assets
cp -r ../assets ./contents/assets

rm -f ./contents/index.md
cp ../README.md ./contents/index.md
sed -i '1i # Introduction' ./contents/index.md

# remove all lines between <!-- GITHUB README ONLY START --> and <!-- GITHUB README ONLY END -->
sed -i '/<!-- GITHUB README ONLY START -->/,/<!-- GITHUB README ONLY END -->/d' ./contents/index.md


# check the first args, should be "serve" or "build"
if [ "$1" != "serve" ] && [ "$1" != "build" ]; then
    echo "Usage: $0 [serve|build]"
    exit 1
fi

DEPS="mkdocs-d2-plugin,mkdocs-material[imaging],mkdocs-add-number-plugin,mkdocs-enumerate-headings-plugin,mkdocs-git-revision-date-localized-plugin,mkdocs-git-committers-plugin-2"

if [ "$1" == "serve" ]; then
    uvx --with "$DEPS" mkdocs serve -a 0.0.0.0:2023
elif [ "$1" == "build" ]; then
    uvx --with "$DEPS" mkdocs build
    cp googlec013d979e435280a.html ./site/
fi
