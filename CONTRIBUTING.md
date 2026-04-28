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

For quick glance without cloning the code, you can use [Ask DeepWiki](https://deepwiki.com/ex-actor/ex-actor) (might not be up-to-date).

For deeper dive we recommend you to use AI coding tools like [Cursor](https://cursor.com/) and [VSCode Copilot](https://code.visualstudio.com/docs/copilot/overview), which can help you ramp up very fast.

**If AI is not smart enough to help you, feel free to ask in [discussions](https://github.com/ex-actor/ex-actor/discussions), we're pleased to help you :-).**

## How to build from source

This project requires C++20. The following compilers are tested in CI:

| Compiler | Tested versions |
|----------|-----------------|
| GCC      | 11 - 14         |
| Clang    | 16 - 18         |
| MSVC     | 14.50 / `_MSC_VER` 1950 (VS 2026) |

```bash
cd ex-actor

# Generate the build directory (requires python3 and CMake)
# If it fails or blocks due to network issues, just retry it.
# Previously downloaded dependencies are kept in ~/.cache/CPM, so they won't be downloaded again.
python3 scripts/regen_build_dir.py

cd build

# Build
cmake --build . --config Release
# Run all tests
ctest -C Release --output-on-failure
# Run a specific test
ctest -C Release --output-on-failure -R basic_api_test

# Format code (Linux/macOS only, requires clang-format and buildifier)
python3 scripts/format.py
```


## Miscellaneous

### Locate the dependency source code

This project uses [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) to manage C++ dependencies, you can locate the source code of a dependency the following methods:

* **From `compile_commands.json`:** grep the package name to find its include path, e.g. `grep -o '[^ ]*stdexec[^ ]*' compile_commands.json | head -1`.
* **With `regen_build_dir.py`:** sources are cached under `~/.cache/CPM/<package>/<hash>/`.
* **With plain CMake (no `CPM_SOURCE_CACHE`):** sources fall back to `build/_deps/<package>-src/`. Setting `CPM_SOURCE_CACHE` is recommended to avoid re-downloads.

### Using Ninja on Windows

By default, the build script uses the Visual Studio generator on Windows. If you prefer Ninja (faster incremental builds, `compile_commands.json` support), you need to run from a **Developer Command Prompt** or **Developer PowerShell** so that `cl.exe` is on PATH:

1. Open **"Developer PowerShell for VS"** (search in Start menu), or run the following in a regular terminal:
   ```powershell
   # Example for VS2026(18) Community
   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
   & "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64
   ```

2. Then generate the build directory with Ninja:
   ```bash
   python scripts/regen_build_dir.py -G Ninja
   ```

Without the developer environment, CMake will fail with `No CMAKE_C_COMPILER could be found` because the Ninja generator cannot locate MSVC on its own.

### Clangd related

`regen_build_dir.py` will generate `compile_commands.json` for you automatically.

You should also add `--compile-commands-dir=<ex_actor_root_folder>` to your `clangd` configuration so that headers in external
folders can be parsed correctly. See <https://clangd.llvm.org/faq#how-do-i-fix-errors-i-get-when-opening-headers-outside-of-my-project-directory>

For VSCode we already set it in `.vscode/settings.json`, you don't need to configure manually.