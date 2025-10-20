# Contributing Guide

First thank you for considering to contribute to `ex_actor`! In order to make the contribution process smoother, please follow the following guidelines:

- For small changes, you can directly open a pull request, we'll review and merge it in short time.

- ⚠️ For non-trivial changes which will cost you a lot time to implement, in order to save both our time, please open an issue to discuss with us first.
By this way we can refine the design in advance and avoid potential duplicate work.

## How to build from source

```bash
cd ex_actor
./scripts/regen_build_dir.sh

cd build
#build
cmake --build . --config Release
# install
cmake --install . --prefix <your_install_prefix>
# test
ctest -C Release --output-on-failure
```

## Clangd related

`regen_build_dir.sh` will generate `compile_commands.json` for you automatically.

You should also add `--compile-commands-dir=<ex_actor_root_folder>` to your `clangd` configuration so that headers in external
folders can be parsed correctly. See <https://clangd.llvm.org/faq#how-do-i-fix-errors-i-get-when-opening-headers-outside-of-my-project-directory>

For VSCode we already set it in `.vscode/settings.json`, you don't need to configure manually.
