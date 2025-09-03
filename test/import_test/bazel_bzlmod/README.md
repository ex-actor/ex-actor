# Using ex_actor with Bazel(bzlmod style)

ex_actor does adapt bzlmod, but is not in the bzlmod registry now. So you need to explicitly provide a `archive_override`

MODULE.bazel:

```bazel
bazel_dep(name = "ex_actor", version = "0.1")
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
cc_binary(
    name = "main",
    srcs = ["main.cc"],
    deps = ["@ex_actor//:ex_actor"],
)
```
