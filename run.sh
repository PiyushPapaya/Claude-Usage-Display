#!/usr/bin/env bash
# Start the Claude usage server on Linux/macOS. The Windows equivalent is
# start.bat. This runs the server in the foreground; use Ctrl-C to stop, or
# see the systemd unit in docs/ for an always-on setup.
set -euo pipefail

cd "$(dirname "$0")"

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "Error: $PYTHON not found. Install Python 3." >&2
  exit 1
fi

if ! "$PYTHON" -c "import flask" >/dev/null 2>&1; then
  echo "Installing dependencies..."
  "$PYTHON" -m pip install -r requirements.txt
fi

echo "Starting server on http://localhost:${USAGE_PORT:-8080}/ (Ctrl-C to stop)"
exec "$PYTHON" server.py
