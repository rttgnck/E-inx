#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/agentisland_state"
BINARY="$BUILD_DIR/AgentIslandStateTest"

# ArduinoJson comes from whichever environment has been built; the parser under
# test has no other dependency, which is the whole reason it can run on the host.
ARDUINOJSON_DIR=""
for candidate in "$ROOT_DIR"/.pio/libdeps/*/ArduinoJson/src; do
  if [ -d "$candidate" ]; then
    ARDUINOJSON_DIR="$candidate"
    break
  fi
done

if [ -z "$ARDUINOJSON_DIR" ]; then
  echo "ArduinoJson not found under .pio/libdeps. Run 'pio run -e gh_release' once first." >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/agentisland_state/AgentIslandStateTest.cpp"
  "$ROOT_DIR/src/agentisland/AgentIslandModel.cpp"
)

CXXFLAGS=(
  -std=gnu++2a
  -O1
  -Wall
  -Wextra
  -I"$ROOT_DIR/src"
  -I"$ARDUINOJSON_DIR"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
