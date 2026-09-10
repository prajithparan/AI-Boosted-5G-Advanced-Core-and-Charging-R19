#!/usr/bin/env bash
# ADR-0335: launch the ops agent's MCP server.
#
# No subject pin, deliberately and safely: this agent has NO subscriber-reading tools, so there is
# no subject to pin. The server enforces that — every PII tool is outside its scope and returns
# "not permitted" with an audit row.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER="$ROOT/build/tools/mcp-server/mcp-server"
[ -x "$SERVER" ] || { echo "build the mcp-server target first: $SERVER" >&2; exit 1; }
exec "$SERVER"
