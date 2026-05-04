#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 (--listen <PORT> [BIND_ADDRESS] | --connect <HOST> <PORT> | <ws://host:port/> | <HOST> <PORT>)" >&2
  exit 1
fi

exec "$ROOT_DIR/build/consumer" "$@"
