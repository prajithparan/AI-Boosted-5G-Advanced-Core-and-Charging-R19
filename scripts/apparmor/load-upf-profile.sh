#!/usr/bin/env bash
# Loads deploy/apparmor/5gc-upf into the host kernel (ADR-0466). Needs root.
# Afterwards: UPF_APPARMOR_PROFILE=5gc-upf docker compose -f deploy/docker/docker-compose.yml up upf
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
sudo apparmor_parser -r -W "$root/deploy/apparmor/5gc-upf"
sudo aa-status 2>/dev/null | grep -q '5gc-upf' && echo "5gc-upf loaded" || echo "5gc-upf not listed by aa-status (aa-status missing?)"
