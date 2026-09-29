#!/usr/bin/env bash
set -euo pipefail

webots_home="${WEBOTS_HOME:-/usr/local/webots}"
runtime_world="${NM_WEBOTS_RUNTIME_WORLD_FILE:?NM_WEBOTS_RUNTIME_WORLD_FILE is required}"
source_world="${NM_WEBOTS_SOURCE_WORLD_FILE:?NM_WEBOTS_SOURCE_WORLD_FILE is required}"

if [[ -s "$runtime_world" ]]; then
  world="$runtime_world"
else
  world="$source_world"
fi

exec /usr/bin/xvfb-run -a -s '-screen 0 1920x1080x24 +extension GLX +render -noreset' \
  "$webots_home/webots" --batch --stdout --stderr --mode=realtime --stream=w3d --port="${NM_WEBOTS_STREAM_PORT:-1234}" "$world"
