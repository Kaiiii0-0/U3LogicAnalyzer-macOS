#!/bin/bash
# Install the u3-logic-analyzer agent skill into every supported coding
# agent on this machine. Safe to re-run: it overwrites the copies.

set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$DIR/u3-logic-analyzer/SKILL.md"
[ -f "$SRC" ] || { echo "SKILL.md not found at $SRC" >&2; exit 1; }

install_to() {
	local dest="$1"
	mkdir -p "$dest/u3-logic-analyzer"
	if cp "$SRC" "$dest/u3-logic-analyzer/SKILL.md"; then
		echo "  installed: $dest/u3-logic-analyzer/"
	fi
}

echo "Installing u3-logic-analyzer skill:"

# Codex CLI
[ -d "$HOME/.codex" ] && install_to "$HOME/.codex/skills"
# OpenCode
[ -d "$HOME/.config/opencode" ] && install_to "$HOME/.config/opencode/skills"
# Generic agents dir (read by several tools)
[ -d "$HOME/.agents" ] && install_to "$HOME/.agents/skills"
# ZCode
[ -d "$HOME/.zcode" ] && install_to "$HOME/.zcode/skills"

echo "Done. Restart the agent (or open a new session) to pick it up."
echo "Companion MCP server (native u3_* tools): see tools/mcp/u3la_mcp/launch.sh"
