#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_binary=$(mktemp /tmp/sps-at-wait-test.XXXXXX)
trap 'rm -f "$test_binary"' EXIT
c++ -std=c++20 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/cellular/stubs -Isrc \
  tests/cellular/at_wait_test.cpp src/cellular/at_engine.cpp -o "$test_binary"
"$test_binary"
