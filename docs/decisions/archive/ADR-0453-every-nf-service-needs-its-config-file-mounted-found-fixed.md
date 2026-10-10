## ADR-0453: every NF service needs its config file mounted -- found, fixed project-wide, and

**Status:** Closed (work pushed; last citing commit f85b0e0 on origin/main, 2026-10-06).
made a standing rule

**Found while finally running ADR-0451's live check.** A fresh `nrf` container, started completely
independently of anything in this session's own work, crashed immediately:
```
terminate called after throwing an instance of 'std::runtime_error'
  what():  nf_config: could not open config file: /build/config/nrf.json
```
`udsf` crashed identically. Checked rather than assumed: **no NF Dockerfile's runtime stage ships
`config/<nf>.json`**, except `amf`, `li-mdf`, and `oam-gui-bff`, which `COPY` it (a second,
inconsistent mechanism -- and even those three would go stale on a config edit without an image
rebuild, unlike every other runtime value in this project, which is designed to be overridable
without one). No compose service mounted it as a volume either. **This was real latent breakage in
~25 of 28 NF services' Docker deployment, pre-existing, unrelated to anything built this session --
only surfaced now because tonight was the first time in a while a genuinely fresh container start
(not a long-lived one from days ago) was exercised for `nrf`/`udsf` specifically.**

**Fix, applied uniformly to every one of the 28 NF/BSS services in `docker-compose.yml`
(including the 3 that already `COPY` it -- one mechanism project-wide, nothing instance-specific
left to remember):**
```yaml
volumes:
  - certs_data:/build/certs
  - ../../config:/build/config:ro
```
A bind mount, not a `COPY`, chosen deliberately: editing `config/<nf>.json` on the host now takes
effect on the next container restart with no image rebuild, matching how `certs_data` already
works and how every other runtime value in this project is meant to behave (ADR-0077's own
no-hardcoded-config rule, applied to the deployment layer this time, not just source code).
Applied mechanically via a one-off script, anchored on the `certs_data:/build/certs` line already
confirmed present in all 28 service blocks; `docker compose config --quiet` validated clean
afterward. Verified for real, not just syntactically: `nrf` and `udsf` both started clean after
the fix (`nrf: listening on https://0.0.0.0:7777`, `udsf: connected to Valkey Cluster`) -- the
other 26 were not individually started tonight (out of scope for this session), but share the
exact same fix for the exact same confirmed root cause.

**Standing rule, user-directed, mandatory, recorded here because this exact class of mistake must
not recur:** every NF/BSS service added to `docker-compose.yml` from this point forward MUST
include `- ../../config:/build/config:ro` in its `volumes:` list from the moment it is written --
not discovered missing later by a crash. This is now part of this project's own Definition of Done
for a new NF's deployment entry (`docs/DECISIONS.md`'s own Definition-of-Done section, Docker
Compose entry item), alongside the existing cert-volume and port-mapping requirements. A new NF's
Dockerfile must NOT rely on `COPY`ing `config/<nf>.json` into the image as a substitute -- the
bind mount is the one, only, project-wide mechanism, so there is nothing per-NF left to
remember or get wrong.

