## ADR-0138: CI workflow -- bump actions/checkout and actions/cache off the deprecated Node 20 runtime

### Context

Every CI job has been emitting a real GitHub Actions warning on every run:

```
##[warning]Node.js 20 is deprecated. The following actions target Node.js 20 but are being forced
to run on Node.js 24: actions/cache@v4, actions/checkout@v4. For more information see:
https://github.blog/changelog/2025-09-19-deprecation-of-node-20-on-github-actions-runners/
```

Root-caused, not assumed: fetched the real `action.yml` for the pinned `@v4` tag of both actions
(`gh api repos/actions/checkout/contents/action.yml?ref=v4` and the `actions/cache` equivalent) --
both declare `runs: using: node20`. That is the literal source of the warning; GitHub is currently
bridging this by forcing execution onto Node 24 anyway (not yet a hard failure), but the warning's
own linked changelog confirms this is a deprecation heading toward removal, not a permanent shim.

Checked the real current major releases via `gh api repos/actions/<name>/releases/latest`:
`actions/checkout` is at `v7.0.1`, `actions/cache` at `v6.1.0`. Fetched each release's own real
`action.yml` (`gh api repos/actions/<name>/contents/action.yml?ref=<tag>`) and confirmed both
declare `runs: using: node24` natively -- upgrading removes the warning at its source rather than
depending on GitHub's forced-runtime bridge.

### Implementation

Every call site in `.github/workflows/ci.yml` (9 total: 3x `actions/checkout`, 6x
`actions/cache`, one of each per job across `build`/`sanitize`/`lint`) bumped from `@v4` to
`actions/checkout@v7` and `actions/cache@v6`. Checked before bumping: every call site in this repo
uses only the most basic, stable inputs -- bare `- uses: actions/checkout@v4` with no `with:`
block at all, and `actions/cache@v4` with only `path`/`key`/`restore-keys` -- the same core inputs
both actions have kept unchanged since `v3`. No call site uses any input, output, or behavior that
changed across the major version jump, so this is a version bump grounded in checking this repo's
actual usage, not a blind major-version jump.

### Testing and verification

YAML syntax validated with `python3 -c "import yaml; yaml.safe_load(...)"`. Real verification of
the fix itself (does the warning actually disappear) happens on the next CI run this commit
triggers -- not asserted here without evidence.

### What this ADR does NOT include

No other workflow changes. Does not address the separately-diagnosed, unrelated
`sanitize (asan-ubsan)` external runner-shutdown pattern (ADR-0124) or the `lint`/`clang-tidy`
step's own highly variable runtime (both real, disclosed, separate issues, not conflated with this
one). If the Node 20 warning persists after this ships, that would mean either a transitive action
this workflow doesn't directly invoke also targets Node 20, or GitHub's own warning surfaced from a
cached/stale evaluation -- not yet observed, would need its own investigation if it occurs.

### Real verification (added after the triggering run)

Pulled the raw job log for the `build` job of the run this commit triggered
(`gh api repos/.../actions/jobs/<id>/logs --allow-escape-sequences`) and grepped for `Node.js 20`
and `forced to run on Node.js 24`: zero matches. Confirmed `actions/checkout@v7` is the version
actually downloaded and executed (`Download action repository 'actions/checkout@v7'` in the log).
The warning is gone. `sanitize (asan-ubsan)` failed again on this same run with the exact ADR-0124
signature -- expected and unrelated, disclosed above as out of scope for this ADR, not a
regression this change caused.

