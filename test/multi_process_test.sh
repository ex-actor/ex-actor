#! /bin/bash

set -e -x

binary=$1

$binary 0 & pid1=$!
$binary 1 & pid2=$!
wait $pid1
r1=$?
wait $pid2
r2=$?
exit $((r1 || r2))