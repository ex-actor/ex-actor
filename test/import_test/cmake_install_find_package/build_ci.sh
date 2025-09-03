#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x


# Clone ex-actor repository
git clone https://github.com/ex-actor/ex-actor.git --depth 1
pushd ex-actor
  if [ -n "$GITHUB_ACTIONS" ]; then
    # For CI test only, checkout to the current commit. You don't need to do this in your project.
    git fetch --depth 1 origin "$GITHUB_SHA"
    git checkout "$GITHUB_SHA"
  fi

  # Build & install ex-actor
  ./scripts/regen_build_dir.sh
  cmake --build build --config Release
  cmake --install build --prefix "${HOME}/.cmake/packages/"
popd

# return to your project and build it
rm -rf build
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_PREFIX_PATH="${HOME}/.cmake/packages/"
cmake --build build --config Release