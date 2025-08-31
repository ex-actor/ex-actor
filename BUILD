package(default_visibility = ["//visibility:public"])

cc_library(
    name = "ex_actor",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    linkopts = [
        "-lpthread",
    ],
    deps = [
        "//external:stdexec",
    ],
)

cc_test(
    name = "basic_api_test",
    srcs = ["test/basic_api_test.cc"],
    deps = [
        ":ex_actor",
        "//external:fmt",
    ],
)
