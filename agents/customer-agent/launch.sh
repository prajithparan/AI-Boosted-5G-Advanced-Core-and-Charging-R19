#!/usr/bin/env bash
# ADR-0333: launch the customer agent's MCP server pinned to ONE subscriber.
#
# The pin is set here, in the environment, before the server starts -- so it is fixed for the life
# of the process and nothing the agent sends over JSON-RPC can widen it. A launcher that forgets
# the argument gets an error, never an unpinned agent with access to every subscriber.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <subscriber-id>    e.g. $0 imsi-999700000000001" >&2
    echo "refusing to start unpinned: an unpinned customer agent can read any subscriber" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER="$ROOT/build/tools/mcp-server/mcp-server"
[ -x "$SERVER" ] || { echo "build the mcp-server target first: $SERVER" >&2; exit 1; }

export MCP_PIN_CUSTOMER_AGENT="$1"
exec "$SERVER"
