#!/usr/bin/env bash
# Run the host-side model tests (test/host) in a throwaway Alpine container: this PC
# has no native compiler. ArduinoJson comes from the PlatformIO library cache, so
# run `pio run` once first.
set -euo pipefail

cd "$(dirname "$0")/.."
ARDUINOJSON="$(ls -d .pio/libdeps/*/ArduinoJson/src | head -n 1)"
[ -n "$ARDUINOJSON" ] || { echo "ArduinoJson not found; run 'pio run' first" >&2; exit 1; }

MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/work" -w /work alpine:3.20 sh -c "
  apk add --no-cache g++ >/dev/null &&
  g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -DARDUINOJSON_ENABLE_STD_STRING=1 \
      -Isrc -I$ARDUINOJSON test/host/test_model.cpp src/App/Mqttp.cpp src/App/Notifications.cpp src/App/Registry.cpp \
      src/Explorer/Manifest.cpp src/Explorer/DeviceView.cpp src/Explorer/Facts.cpp src/Explorer/Watch.cpp src/Explorer/TopicTap.cpp src/Explorer/Ping.cpp -o /tmp/test_model &&
  /tmp/test_model"
