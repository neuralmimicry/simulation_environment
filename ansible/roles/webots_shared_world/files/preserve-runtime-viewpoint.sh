#!/usr/bin/env bash
set -euo pipefail

source_world="${1:?usage: preserve-runtime-viewpoint.sh SOURCE_WORLD RUNTIME_WORLD}"
runtime_world="${2:?usage: preserve-runtime-viewpoint.sh SOURCE_WORLD RUNTIME_WORLD}"
temporary_world="${runtime_world}.viewpoint.pending"

[[ -s "$source_world" ]] || { echo "source world is missing: $source_world" >&2; exit 1; }
[[ -s "$runtime_world" ]] || { echo "runtime snapshot is missing: $runtime_world" >&2; exit 1; }

awk '
  function is_viewpoint_start(line) {
    return line ~ /^[[:space:]]*Viewpoint[[:space:]]*\{/
  }
  function is_node_end(line) {
    return line ~ /^[[:space:]]*\}[[:space:]]*$/
  }
  FNR == NR {
    if (is_viewpoint_start($0)) in_source_view = 1
    if (in_source_view) {
      source_view = source_view $0 ORS
      if (is_node_end($0)) {
        in_source_view = 0
        source_view_found = 1
      }
    }
    next
  }
  {
    if (!in_runtime_view && is_viewpoint_start($0)) {
      in_runtime_view = 1
      runtime_view_found = 1
      printf "%s", source_view
      if (is_node_end($0)) in_runtime_view = 0
      next
    }
    if (in_runtime_view) {
      if (is_node_end($0)) in_runtime_view = 0
      next
    }
    print
  }
  END {
    if (!source_view_found) {
      print "source world has no complete Viewpoint node" > "/dev/stderr"
      exit 2
    }
    if (!runtime_view_found) {
      print "runtime snapshot has no Viewpoint node" > "/dev/stderr"
      exit 3
    }
  }
' "$source_world" "$runtime_world" > "$temporary_world"

chown --reference="$runtime_world" "$temporary_world"
chmod --reference="$runtime_world" "$temporary_world"
mv -f "$temporary_world" "$runtime_world"
