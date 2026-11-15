#!/bin/bash
# u3la-mcp launcher — stdio MCP server for the U3 logic analyzer.
# Zero Python dependencies; any CPython >= 3.9 works. Prefer the brew
# python3.12 the repo already depends on, fall back to system python3.
#
# Registered as an MCP server via the command + args below:
#   command: /path/to/repo/tools/mcp/u3la_mcp/launch.sh
#   args:    []   (stdio transport)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"

PY="python3"
for cand in /opt/homebrew/bin/python3.12 /opt/homebrew/opt/python@3.12/bin/python3.12 python3; do
    if command -v "$cand" >/dev/null 2>&1; then
        PY="$cand"
        break
    fi
done

export U3LA_REPO="$REPO"
export U3CTL_BIN="${U3CTL_BIN:-$REPO/build_u3ctl/u3ctl}"

exec "$PY" "$SCRIPT_DIR/server.py"
