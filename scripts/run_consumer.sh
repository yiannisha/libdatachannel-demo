#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <ws://producer-host:port/>" >&2
  exit 1
fi

exec "$ROOT_DIR/build/consumer" "$1"
