#! /bin/bash

SRC=$(
    cd "$(dirname "$0")" || exit
    pwd
)
cd "$SRC" || exit
set -e -x

pushd "${GITHUB_WORKSPACE}"/..
  zip -r /tmp/code_after_merge.zip ex-actor
  zip_sha256=$(sha256sum /tmp/code_after_merge.zip | awk '{print $1}')
popd

{
  echo "http_archive("
  echo "    name = \"ex_actor\","
  echo "    strip_prefix = \"ex-actor\","
  echo "    urls = [\"file:///tmp/code_after_merge.zip\"],"
  echo "    sha256 = \"${zip_sha256}\","
  echo ")"
} >> WORKSPACE

echo "Final WORKSPACE:"
cat WORKSPACE

bazel build //:main