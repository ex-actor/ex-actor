# Contributing Guide

First thank you for considering to contribute to `ex_actor`!

## Have questions about any part of the project?

Participating in this project is not limited to writing code, your usage and feedback are also invaluable contributions!

If you find any bugs or have any feature requests, open an issue to let us know, we'll try our best to help you.

## Want to implement something yourself?

Have a look at existing issues and pick what you're interested in, especially those with "good first issue" label, such issues are relatively easy to implement.

If you have your own ideas:

* For small changes, you can directly open a pull request without an issue, we'll review and merge it in a short time.
* ⚠️ For non-trivial changes which will cost you a lot of time to implement, in order to save both our time, please open an issue to discuss with us first. This way we can refine the design in advance and avoid potential duplicate work.

## How to build from source

## Linux

```bash
cd ex-actor

# If it fails due to network issues, just retry it.
# In this script I set the cache dir for CPM; previously successful downloads will be cached.
./scripts/regen_build_dir.sh

cd build
# Build
cmake --build . --config Debug
# Run all tests
ctest -C Debug --output-on-failure
# Run specific test
./test/Debug/basic_api_test

# Format code
./scripts/format.sh
```

## Windows

```powershell
cd ex-actor
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug

# Run all tests
ctest -C Debug --output-on-failure
```

## Clangd related

`regen_build_dir.sh` will generate `compile_commands.json` for you automatically.

You should also add `--compile-commands-dir=<ex_actor_root_folder>` to your `clangd` configuration so that headers in external
folders can be parsed correctly. See <https://clangd.llvm.org/faq#how-do-i-fix-errors-i-get-when-opening-headers-outside-of-my-project-directory>

For VSCode we already set it in `.vscode/settings.json`, you don't need to configure manually.
