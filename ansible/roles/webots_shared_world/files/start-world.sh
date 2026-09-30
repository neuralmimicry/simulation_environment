#!/usr/bin/env bash
set -euo pipefail

webots_home="${WEBOTS_HOME:-/usr/local/webots}"
runtime_world="${NM_WEBOTS_RUNTIME_WORLD_FILE:?NM_WEBOTS_RUNTIME_WORLD_FILE is required}"
source_world="${NM_WEBOTS_SOURCE_WORLD_FILE:?NM_WEBOTS_SOURCE_WORLD_FILE is required}"

source_dir="$(cd "$(dirname "$source_world")/.." && pwd)"
legacy_source_revision="$(
  {
    sha256sum "$source_world"
    find "$source_dir/protos" -maxdepth 1 -type f -name '*.proto' -print0 \
      | sort -z \
      | xargs -0 -r sha256sum
  } | sha256sum | cut -d' ' -f1
)"
binding_revision="${NM_WEBOTS_BINDINGS_REVISION:-unconfigured}"
source_revision="$(printf '%s\n%s\n' "$legacy_source_revision" "$binding_revision" | sha256sum | cut -d' ' -f1)"

if [[ -s "$runtime_world" ]]; then
  revision_file="${runtime_world}.source-revision"
  saved_revision=""
  [[ -r "$revision_file" ]] && saved_revision="$(<"$revision_file")"
  if [[ "$saved_revision" == "$source_revision" ]]; then
    world="$runtime_world"
  elif [[ "$saved_revision" == "$legacy_source_revision" ]]; then
    # Keep the saved ecology and robot poses during the first deployment of
    # binding-aware robot selection; the supervisor will prune only unbound slots.
    world="$runtime_world"
  else
    # A changed source world or PROTO graph needs a fresh Webots scene. Keep
    # the previous snapshot recoverable; the ecology supervisor preserves
    # elapsed world time independently and marks the new snapshot after save.
    previous_revision="${saved_revision:-legacy}"
    previous_revision="${previous_revision:0:12}"
    backup_world="${runtime_world}.pre-${previous_revision}"
    if [[ ! -e "$backup_world" ]]; then
      cp -a --reflink=auto "$runtime_world" "$backup_world"
    fi
    world="$source_world"
  fi
else
  world="$source_world"
fi

export NM_WEBOTS_SOURCE_REVISION="$source_revision"
exec /usr/bin/xvfb-run -a -s '-screen 0 1920x1080x24 +extension GLX +render -noreset' \
  "$webots_home/webots" --batch --stdout --stderr --mode=realtime --stream=w3d --port="${NM_WEBOTS_STREAM_PORT:-1234}" "$world"
