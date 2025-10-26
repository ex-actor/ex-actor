# Installation

Can't find your build system? Open an issue to let us know. Welcome to open a PR to contribute!

## CMake Projects

### Use cmake package manager ([CPM](https://github.com/cpm-cmake/CPM.cmake)) (Recommended)

Run the following commands to get CPM.cmake, it's a wrapper of CMake's `FetchContent`:
```bash
wget -O CPM.cmake https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake
```

Then you can use `CPMAddPackage` to add `ex_actor` as your dependency:

```cmake
cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(my_project)

# For more information on how to add CPM to your project, see: https://github.com/cpm-cmake/CPM.cmake#adding-cpm
include(CPM.cmake)

CPMAddPackage(
  NAME ex_actor
  GITHUB_REPOSITORY ex-actor/ex-actor
  GIT_TAG main # This will always pull the latest code from the `main` branch. You may also use a specific commit ID
  OPTIONS "EX_ACTOR_BUILD_TESTS OFF"
)

add_executable(main main.cc)
target_link_libraries(main ex_actor::ex_actor)

```

**Highly recommend you to `export CPM_SOURCE_CACHE=$HOME/.cache/CPM` to set a cache directory for CPM.cmake.**
**It's useful when you fail the download due to network issues, and want to retry again. With this you don't need to download the successfully downloaded package again.**

### Use legacy install and find_package

first, install ex_actor to your system:

```bash
git clone https://github.com/ex-actor/ex-actor.git --depth 1 && cd ex-actor

# Set a cache directory for CPM.cmake, It's useful when you fail the download due
# to network issues, and want to retry again. Avoid re-downloading.
export CPM_SOURCE_CACHE=$HOME/.cache/CPM

cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Release
cmake --install build --prefix <your_install_prefix>
```

then, you can use `find_package` to add ex_actor as your dependencies.

```cmake
find_package(ex_actor REQUIRED)
```

don't forget to set `CMAKE_PREFIX_PATH` to your install prefix.
```bash
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_PREFIX_PATH=<your_install_prefix>
cmake --build build --config Release
```

## Bazel Projects

### Bzlmod style

ex_actor does adapt bzlmod, but is not in the bzlmod registry now. So you need to explicitly provide a `archive_override`

MODULE.bazel:

```bazel
bazel_dep(name = "ex_actor", version = "0.1")
bazel_dep(name = "rules_cc", version = "0.2.3")
archive_override(
    module_name = "ex_actor",

    # replace <latest_commit_id> with the latest commit ID.
    strip_prefix = "ex-actor-<latest_commit_id>",
    urls = ["https://github.com/ex-actor/ex-actor/archive/<latest_commit_id>.zip"],

    # Download zip file to your local machine, run `sha256sum <zip_file_name>` to get the sha256 of the zip file.
    sha256 = "<sha256_of_the_zip_file>",
)
```

You should find the latest commit ID from [ex-actor commit list page](https://github.com/ex-actor/ex-actor/commits/main).

The zip url should be: `https://github.com/ex-actor/ex-actor/archive/<latest_commit_id>.zip`,
note the commit ID should be a full commit ID like `acd385cd467253385b44b382a166b4a24ce8cbaa`, not a short ID like `acd385c`.

Finally, use can `@ex_actor//:ex_actor` as your dependencies.

```bazel
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
cc_binary(
    name = "main",
    srcs = ["main.cc"],
    deps = ["@ex_actor"],
)
```

### Legacy WORKSPACE style

First, add [rules_foreign_cc](https://github.com/bazel-contrib/rules_foreign_cc) to your WORKSPACE file.

```bazel
# ------------------------ rules_foreign_cc setup ------------------------
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(
    name = "rules_foreign_cc",
    sha256 = "32759728913c376ba45b0116869b71b68b1c2ebf8f2bcf7b41222bc07b773d73",
    strip_prefix = "rules_foreign_cc-0.15.1",
    url = "https://github.com/bazel-contrib/rules_foreign_cc/releases/download/0.15.1/rules_foreign_cc-0.15.1.tar.gz",
)
load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")
# This sets up some common toolchains for building targets. For more details, please see
# https://bazel-contrib.github.io/rules_foreign_cc/0.15.1/flatten.html#rules_foreign_cc_dependencies
rules_foreign_cc_dependencies()
# If you're not already using bazel_skylib, bazel_features or rules_python,
# you'll need to add these calls as well.
load("@bazel_skylib//:workspace.bzl", "bazel_skylib_workspace")
bazel_skylib_workspace()
load("@bazel_features//:deps.bzl", "bazel_features_deps")
bazel_features_deps()
load("@rules_python//python:repositories.bzl", "py_repositories")
py_repositories()
load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")
protobuf_deps()
```

Then, add `ex_actor`'s source code as `http_archive` to your WORKSPACE file.

The zip url should be: `https://github.com/ex-actor/ex-actor/archive/<latest_commit_id>.zip`.

You should find the latest commit ID from [ex-actor commit list page](https://github.com/ex-actor/ex-actor/commits/main).
Note the commit ID should be a full commit ID like `acd385cd467253385b44b382a166b4a24ce8cbaa`, not a short ID like `acd385c`.

```bazel
http_archive(
    name = "ex_actor",

    # replace <latest_commit_id> with the latest commit ID.
    strip_prefix = "ex-actor-<latest_commit_id>",
    urls = ["https://github.com/ex-actor/ex-actor/archive/<latest_commit_id>.zip"],

    # Download zip file to your local machine, run `sha256sum <zip_file_name>` to get the sha256 of the zip file.
    sha256 = "<sha256_of_the_zip_file>",
)
```

Finally, use `@ex_actor` as your dependencies.

```bazel
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
cc_binary(
    name = "main",
    srcs = ["main.cc"],
    deps = ["@ex_actor"],
)
```

## Dependencies

Our dependencies are listed in root directory's `CMakeLists.txt` (search for `CPMAddPackage`).
All of them will be automatically downloaded and built from source, you don't need to configure them manually.

We know it's a headache to resolve dependency conflicts, so we carefully choose minimal dependencies.
If you meet any dependency conflict, either try to modify our version in CMakeLists.txt to match your project, or modify your version to match ours.
Welcome to open a issue to let us know if have any problem.