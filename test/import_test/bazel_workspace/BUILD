load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

cmake(
    name = "ex_actor",
    lib_source = "@ex_actor//:all_srcs",
    cache_entries = {
      "EX_ACTOR_BUILD_TESTS": "OFF",
    },
    out_headers_only = True,
)

cc_binary(
    name = "main",
    srcs = ["main.cc"],
    deps = [":ex_actor"],
)