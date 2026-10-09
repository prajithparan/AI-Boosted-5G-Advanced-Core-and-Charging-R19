#!/usr/bin/env bash
# Refuse to run local integration tests or Compose services while the CI runner on this host is
# executing a job that may use the same fixed loopback ports (NRF 7777, NWDAF 7797, UPF 7796/8805, ...).
# Local ctest or `docker compose up` during CI's Test step made 9 CI tests fail on 2026-10-09 (run
# 37875375075 attempt 1). Usage: scripts/local-test-guard.sh && ctest ...   (exit 0 = safe)
set -u
repo="${GH_REPO:-$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null)}"
busy=$(gh api "repos/${repo}/actions/runners" --jq '[.runners[]|select(.busy==true)]|length' 2>/dev/null)
if [ -z "${busy}" ]; then
    echo "local-test-guard: cannot read runner state from GitHub; refusing to guess (exit 2)" >&2
    exit 2
fi
if [ "${busy}" != "0" ]; then
    echo "local-test-guard: the CI runner is busy; local tests would collide on loopback ports. Wait (exit 1)." >&2
    exit 1
fi
holders=$(ss -lntuH 2>/dev/null | grep -E ":(7777|7796|7797|8805|9464|9471|19994) " || true)
if [ -n "${holders}" ]; then
    echo "local-test-guard: something already listens on a test port:" >&2
    echo "${holders}" >&2
    exit 1
fi
exit 0
