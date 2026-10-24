#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build=${1:-build-ota}
zbase=${ZEPHYR_BASE:-$(sed -n 's/^ZEPHYR_BASE:[^=]*=//p' "$build/CMakeCache.txt" | head -n 1)}
tmp=$(mktemp -d /tmp/sps-http-ota-test.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
python3 - "$build" "$tmp" <<'PY'
import sys,zipfile
from pathlib import Path
with zipfile.ZipFile(Path(sys.argv[1])/'dfu_application.zip') as z:
    Path(sys.argv[2],'manifest.json').write_bytes(z.read('manifest.json'))
PY
cc -std=c11 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -include inttypes.h -Itests/http_ota/stubs -I"$zbase/include" \
  -c "$zbase/lib/utils/json.c" -o "$tmp/json.o"
c++ -std=c++20 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/http_ota/stubs -Isrc -I"$zbase/include" -I"$build/smart_power_socket/ota_generated" \
  tests/http_ota/manifest_test.cpp tests/http_ota/crypto_adapter.cpp \
  src/ota/manifest.cpp src/ota/image_verify.cpp "$tmp/json.o" -lcrypto -o "$tmp/manifest_test"
if [ "$#" -ge 3 ]; then
  "$tmp/manifest_test" "$tmp/manifest.json" "$build/smart_power_socket/zephyr/zephyr.signed.bin" "$2" "$3"
elif [ "$#" -ge 2 ]; then
  "$tmp/manifest_test" "$tmp/manifest.json" "$build/smart_power_socket/zephyr/zephyr.signed.bin" "$2"
else
  "$tmp/manifest_test" "$tmp/manifest.json" "$build/smart_power_socket/zephyr/zephyr.signed.bin"
fi
c++ -std=c++20 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  '-DCONFIG_SPS_OTA_BASE_URL="https://raw.githubusercontent.com/test/test/main/dfu_application/"' \
  -Itests/http_ota/controller_stubs -Itests/http_ota/stubs -Isrc -I"$zbase/include" \
  tests/http_ota/controller_test.cpp tests/http_ota/crypto_adapter.cpp \
  src/ota/http_update.cpp src/ota/manifest.cpp "$tmp/json.o" -lcrypto -o "$tmp/controller_test"
"$tmp/controller_test" "$tmp/manifest.json" "$build/smart_power_socket/zephyr/zephyr.signed.bin"
c++ -std=c++20 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/http_ota/stubs -Isrc -Imodules/sps_boot/src -I"$build/smart_power_socket/ota_generated" \
  tests/http_ota/writer_test.cpp tests/http_ota/crypto_adapter.cpp \
  src/ota/firmware_update.cpp src/ota/image_verify.cpp -lcrypto -o "$tmp/writer_test"
"$tmp/writer_test" "$build/smart_power_socket/zephyr/zephyr.signed.bin"
if [ "$#" -ge 2 ]; then
  "$tmp/writer_test" "$build/smart_power_socket/zephyr/zephyr.signed.bin" "$2"
fi
if [ "$#" -ge 3 ]; then
  "$tmp/writer_test" "$build/smart_power_socket/zephyr/zephyr.signed.bin" "$3"
fi
# Exercise the longest representable MCUboot version together with max counters.
c++ -std=c++20 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  '-DAPP_VERSION_TWEAK_STRING="255.255.65535+4294967295"' \
  -Itests/http_ota/stubs -Isrc -Imodules/sps_boot/src -I"$build/smart_power_socket/ota_generated" \
  tests/http_ota/writer_test.cpp tests/http_ota/crypto_adapter.cpp \
  src/ota/firmware_update.cpp src/ota/image_verify.cpp -lcrypto -o "$tmp/writer_long_version_test"
"$tmp/writer_long_version_test" "$build/smart_power_socket/zephyr/zephyr.signed.bin"
