# Contributing Guide

First thank you for considering to contribute to `ex_actor`!

## Have Questions about any part of the project?

Open an issue, we'll try our best to help you!

## Bugs & Features Requests

If you find any bugs or have any feature requests, welcome to open an issue to let us know!

For small changes, you can directly open a pull request without an issue, we'll review and merge it in short time.

⚠️ For non-trivial changes which will cost you a lot time to implement, in order to save both our time, please open an issue to discuss with us first.
By this way we can refine the design in advance and avoid potential duplicate work.

## Roadmap

You can find our roadmap [here](https://github.com/orgs/ex-actor/projects/2). That's what we want to achieve in the future.

Have a look and pick what you're interested in, discuss with us in the related issue, and start coding!

If you have other ideas, welcome to open an issue to discuss with us!

## How to build from source

```bash
cd ex_actor
./scripts/regen_build_dir.sh

cd build
# build
cmake --build . --config Debug
# run all test
ctest -C Debug --output-on-failure
# run specific test
./test/Debug/basic_api_test
```

## Clangd related

`regen_build_dir.sh` will generate `compile_commands.json` for you automatically.

You should also add `--compile-commands-dir=<ex_actor_root_folder>` to your `clangd` configuration so that headers in external
folders can be parsed correctly. See <https://clangd.llvm.org/faq#how-do-i-fix-errors-i-get-when-opening-headers-outside-of-my-project-directory>

For VSCode we already set it in `.vscode/settings.json`, you don't need to configure manually.
