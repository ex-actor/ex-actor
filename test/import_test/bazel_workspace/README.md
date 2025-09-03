# Using ex_actor with Bazel(WORKSPACE style)

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

You should find the latest commit ID from [ex-actor commit list page](https://github.com/ex-actor/ex-actor/commits/main).

The zip url should be: `https://github.com/ex-actor/ex-actor/archive/<latest_commit_id>.zip`

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

Finally, use `@ex_actor//:ex_actor` as your dependencies.

```bazel
cc_binary(
    name = "main",
    srcs = ["main.cc"],
    deps = ["@ex_actor//:ex_actor"],
)
```