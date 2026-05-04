#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -f "$ROOT_DIR/gst_zed_plugin.sh" ]]; then
  # Keep the ZED SDK and GStreamer plugin path aligned with the local gst-test build.
  # shellcheck disable=SC1091
  source "$ROOT_DIR/gst_zed_plugin.sh"
fi

if [[ $# -eq 0 ]]; then
  exec "$ROOT_DIR/build/producer" --listen 8080 0.0.0.0 default
fi

exec "$ROOT_DIR/build/producer" "$@"
