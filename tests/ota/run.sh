#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_binary=$(mktemp /tmp/sps-recovery-test.XXXXXX)
trap 'rm -f "$test_binary"' EXIT
cc -std=c11 -g -DCONFIG_MCUBOOT=1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/ota/stubs -Imodules/sps_boot/src \
  tests/ota/recovery_test.c modules/sps_boot/src/boot_hooks.c \
  modules/sps_boot/src/boot_storage.c -o "$test_binary"
"$test_binary" "$@"
