#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_binary=$(mktemp /tmp/sps-task-watchdog-test.XXXXXX)
trap 'rm -f "$test_binary"' EXIT
c++ -std=c++20 -g -DCONFIG_TASK_WDT=1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/watchdog/stubs -Isrc -Imodules/sps_boot/src \
  tests/watchdog/watchdog_test.cpp src/system/watchdog.cpp \
  modules/sps_boot/src/watchdog_reset.c -o "$test_binary"
"$test_binary"
