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

  LAUNCHER_FLAGS=()
  if command -v ccache &>/dev/null; then
    LAUNCHER_FLAGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  fi

  # Build & install ex-actor
  python3 scripts/regen_build_dir.py "${LAUNCHER_FLAGS[@]}"
  cmake --build build --config Release
  cmake --install build --prefix "${HOME}/.cmake/packages/"
popd

# return to your project and build it
LAUNCHER_FLAGS=()
if command -v ccache &>/dev/null; then
  LAUNCHER_FLAGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

rm -rf build
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_PREFIX_PATH="${HOME}/.cmake/packages/" "${LAUNCHER_FLAGS[@]}"
cmake --build build --config Release