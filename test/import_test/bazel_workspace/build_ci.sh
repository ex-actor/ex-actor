#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

pushd "${GITHUB_WORKSPACE}"/..
  zip -r /tmp/code_after_merge.zip ex-actor -x "*.git*"
  zip_sha256=$(sha256sum /tmp/code_after_merge.zip | awk '{print $1}')
popd

{
  echo "load(\"@bazel_tools//tools/build_defs/repo:http.bzl\", \"http_archive\")"
  echo "http_archive("
  echo "    name = \"ex_actor\","
  echo "    strip_prefix = \"ex-actor\","
  echo "    urls = [\"file:///tmp/code_after_merge.zip\"],"
  echo "    sha256 = \"${zip_sha256}\","
  echo ")"
} >> WORKSPACE.bazel

echo "Final WORKSPACE.bazel:"
cat WORKSPACE.bazel

bazel build //:main
bazel clean