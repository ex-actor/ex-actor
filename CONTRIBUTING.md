# Contributing Guide

Thank you for considering contributing to `ex_actor`!

Firstly, participating in this project is not limited to writing code, **your feedback and discussions with others are also invaluable contributions**.

* If you meet any bugs or have any feature requests, don't hesitate to [open an issue](https://github.com/ex-actor/ex-actor/issues) to let us know.

* For casual questions and discussions, feel free to open a post in [discussions page](https://github.com/ex-actor/ex-actor/discussions).

We'll try our best to help you!



## Want to implement something yourself?

Have a look at existing issues and pick what you're interested in, especially those with "good first issue" label, such issues are relatively easy to implement.

If you have your own ideas:

* For small changes, you can directly open a pull request without an issue, we'll review and merge it in a short time.
* ⚠️ For non-trivial changes which will cost you a lot of time to implement, in order to save both our time, please open an [issue](https://github.com/ex-actor/ex-actor/issues) or [discussion](https://github.com/ex-actor/ex-actor/discussions) to discuss with us first. By this way we can refine the design in advance and avoid potential duplicate work.

## Understand the code structure

It's AI Era bro, we recommend you use AI tools like [Ask DeepWiki](https://deepwiki.com/ex-actor/ex-actor), or ask chat bot like ChatGPT&Gemini directly, which really helps.

When coding, we also highly recommend you to use AI coding tools like Cursor, VSCode Copilot, Claude Code etc, which can help you ramp up much faster.

**If AI is not smart enough to help you, feel free to ask in [discussions](https://github.com/ex-actor/ex-actor/discussions), we're pleased to help you :-).**

## How to build from source

### Linux

```bash
cd ex-actor

# If it fails due to network issues, just retry it.
# In this script I set the cache dir for CPM; previously successful downloads will be cached.
./scripts/regen_build_dir.sh

pushd build
# Build
cmake --build . --config Debug

# Run all tests
ctest -C Debug --output-on-failure

# Run specific test
# option 1, handy for test without arguments
./test/Debug/basic_api_test
# option 2, for test with arguments/wrapper script
ctest -C Debug --output-on-failure -R multi_process_test

# Format code
./scripts/format.sh
```

### Windows

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
