#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-8080}"
BIND_ADDRESS="${2:-0.0.0.0}"

exec "$ROOT_DIR/build/producer" "$PORT" "$BIND_ADDRESS"
