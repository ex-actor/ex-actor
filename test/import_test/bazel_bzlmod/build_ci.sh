#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

pushd "${GITHUB_WORKSPACE}"/..
  zip -r /tmp/code_after_merge.zip ex-actor -x "*.git*" -x "*build*" -x "*bazel-*" -x "*bazel*external" -x "*.cache*"
  zip_sha256=$(sha256sum /tmp/code_after_merge.zip | awk '{print $1}')
popd

{
  echo "archive_override("
  echo "    module_name = \"ex_actor\","
  echo "    strip_prefix = \"ex-actor\","
  echo "    urls = [\"file:///tmp/code_after_merge.zip\"],"
  echo "    sha256 = \"${zip_sha256}\","
  echo ")"
} >> MODULE.bazel

echo "Final MODULE.bazel:"
cat MODULE.bazel

bazel run @ex_actor//:distributed_test
bazel build //:main
bazel clean