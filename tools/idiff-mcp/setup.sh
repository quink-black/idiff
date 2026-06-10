#!/usr/bin/env bash
# tools/idiff-mcp/setup.sh -- provision the local venv for the idiff
# MCP server.  Idempotent: re-running just upgrades dependencies.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

if ! command -v python3 &>/dev/null; then
    echo "❌ python3 not found in PATH"
    exit 1
fi

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating venv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
fi

source "$VENV_DIR/bin/activate"
pip install --upgrade pip >/dev/null
pip install -r "$SCRIPT_DIR/requirements.txt"

echo ""
echo "✅ idiff-mcp ready."
echo "   venv:  $VENV_DIR"
echo "   entry: $SCRIPT_DIR/idiff_mcp_server.py"
echo ""
echo "Add this to ~/.codebuddy/mcp.json under mcpServers:"
echo
cat <<EOF
  "idiff": {
    "command": "$VENV_DIR/bin/python",
    "args": ["$SCRIPT_DIR/idiff_mcp_server.py"],
    "env": {}
  }
EOF
