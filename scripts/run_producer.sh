#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-8080}"
BIND_ADDRESS="${2:-0.0.0.0}"
PIPELINE_PROFILE="${3:-default}"

if [[ -f "$ROOT_DIR/gst_zed_plugin.sh" ]]; then
  # Keep the ZED SDK and GStreamer plugin path aligned with the local gst-test build.
  # shellcheck disable=SC1091
  source "$ROOT_DIR/gst_zed_plugin.sh"
fi

exec "$ROOT_DIR/build/producer" "$PORT" "$BIND_ADDRESS" "$PIPELINE_PROFILE"
