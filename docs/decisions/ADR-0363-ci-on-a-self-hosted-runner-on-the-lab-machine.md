## ADR-0363: CI on a self-hosted runner on the lab machine

**Date:** 2026-09-14. **Status:** accepted. User-directed ("set up the self-hosted runner").

**Why.** GitHub's free-tier runners (2 vCPU, 7 GB) forced the `-j2`/`-j1` build caps of
ADR-0222/0225/0229, made a run 1.5-2 h, and were reclaimed mid-job often enough (ADR-0124) that
four of the eight runs before this one never reached the Test step -- which is how a shutdown
regression from 2026-09-13 stayed invisible until 2026-09-14 (ADR-0362). The lab machine has 8
cores and the whole toolchain already installed.

**What.** One runner (`actions/runner` 2.337.0, user-level in `~/actions-runner`, labels
`self-hosted,linux,x64,5gc-lab`) registered against the repository through the API. The
workflow's host-dependent steps are conditional on `runner.environment`, so flipping `runs-on`
back to `ubuntu-latest` restores the previous behaviour unchanged:

- `apt-get` and `actions/cache` steps run only on GitHub-hosted runners; a self-hosted run
  instead verifies the toolchain is present and fails loudly if not.
- ccache (4.14, static binary in `~/.local/bin`, 20 GB cap), the vcpkg checkout and its binary
  cache, and the asn1c toolchain live under `~/.cache/5gc-ci/`, outside the per-run workspace
  that `actions/checkout` cleans; `build-tools/` is a symlink into it.
- Parallelism is `-j4` on the lab box (15 GB: `-j8` was measured to thrash), `-j2`/`-j1` on
  GitHub-hosted as before.
- Service containers publish on host ports +10000 (Postgres 15432-15437, Valkey 16379, Kafka
  19092, Doris 19030/18030/18040) so the developer's lab compose stack can stay up on the
  standard ports while CI runs; every NF's datastore env override is set in the Test steps.
- Schema-apply steps `docker exec` into the service containers, so no `psql`/`mysql` on the
  host; identical on both runner kinds.
- `concurrency: cancel-in-progress` per ref: a newer push cancels the run it supersedes, which
  is what a single serial runner needs.

**Disclosed.** The runner is started with `nohup` because installing it as a systemd service
needs `sudo` (`sudo ./svc.sh install && sudo ./svc.sh start` in `~/actions-runner`, to be run by
the user); until then it does not survive a reboot. One runner means the three jobs run one
after another. CI now shares the machine with development: a local build during a CI run slows
both. A runner on the developer's box is not a substitute for the hardened, isolated CI a
production release process needs; it is the right tool for this phase.

