## ADR-0325: ten NEF API roots were invented, and no test could see it

**Date:** 2026-09-09. **Status:** Closed (work pushed; last citing commit 2fde5b2 on origin/main, 2026-10-06). accepted. Correction to ADR-0322, ADR-0323 and ADR-0324.

While enumerating the AF services still to be built, a script compared each routed API root
against its YAML's `servers[0].url` and found **ten of the twenty AF-facing roots matched no
specification in the repository**. They were not typos. They were derived from the spec
*filename*:

| Spec | Root that was routed | Root the YAML declares |
|---|---|---|
| TS29522_CagInfoParamProvision | `/3gpp-cag-info-provision/v1` | `/3gpp-caginfo-pp/v1` |
| TS29522_AddressingParamProvision | `/3gpp-addressing-param-provision/v1` | `/3gpp-addr-pp/v1` |
| TS29522_SliceParamProvision | `/3gpp-slice-param-provision/v1` | `/3gpp-slice-pp/v1` |
| TS29522_GroupParametersProvisioning | `/3gpp-group-parameter-provision/v1` | `/3gpp-grp-pp/v1` |
| TS29522_ACSParameterProvision | `/3gpp-acs-parameter-provision/v1` | `/3gpp-acs-pp/v1` |
| TS29522_LpiParameterProvision | `/3gpp-lpi-parameter-provision/v1` | `/3gpp-lpi-pp/v1` |
| TS29522_IPTVConfiguration | `/3gpp-iptv-configuration/v1` | `/3gpp-iptvconfiguration/v1` |
| TS29522_ImsEventExposure | `/3gpp-ims-event-exposure/v1` | `/3gpp-ims-ee/v1` |
| TS29122_RacsParameterProvisioning | `/3gpp-racs-parameter-provisioning/v1` | `/3gpp-racs-pp/v1` |
| TS29122_ReportingNetworkStatus | `/3gpp-network-status/v1` | `/3gpp-net-stat-report/v1` |

3GPP does not derive the root from the filename, and the pattern of abbreviation (`-pp`, `ims-ee`,
`net-stat-report`) is not predictable from it. Every one of these was a guess that read as a fact.
No AF built against TS 29.522 would have reached any of these ten services.

Two further defects the same comparison exposed: `Nnef_UEId`'s provisioning collection was routed
at `/{afId}/provisionings/{provisioningId}` where the spec says `/{afId}/pp/{ppId}`, and two
operations were missing outright -- `FetchAnalyticsInfo` (`POST /{afId}/fetch`) and
`RetrieveStatusofConfiguration` (`POST /{afId}/configurations/retrieve`). ADR-0324 claimed the
method sets were read per service, and they were, but only for the paths the six-operation shape
expects; a third path in each spec went unread. The correction to a habit was itself applied
habitually.

### Why 580 passing tests said nothing

A test that builds its request URL from the same constant the server registers is self-consistent
by construction. It cannot detect that the constant disagrees with the specification, because the
specification is nowhere in its loop. Every NEF test did exactly this and all of them passed. The
number of passing tests was never evidence about roots and never will be.

### The fix that is not the ten strings

The roots are corrected, `/{afId}/pp` is corrected, and the two missing operations are added
(returning 501 against a parsed, real request DTO -- `AnalyticsData` has to come from NWDAF and
the ASTI status from TSCTSF, neither wired here, and synthesising either is the one failure this
project cannot afford). But the durable fix is `tests/conformance/validate_api_roots.py`, run as
the `api_root_conformance` ctest: it walks every `nfs/*` and `bss/*` source, extracts every API
root constant, and requires each to equal some R19 YAML's `servers[0].url`.

It is repo-wide, not NEF-scoped, deliberately -- a NEF-only check would pass while leaving the
same class of defect live everywhere else. Run across the whole tree it found 96 root constants,
of which the only non-conforming ones outside NEF are five TM Forum and project roots that have no
3GPP YAML by definition; those are named in an allowlist that may not be extended to silence a
`/3gpp-` or `/n<nf>-` mismatch. The check was confirmed to fail when a root is reverted to its
wrong value, and to fail rather than pass vacuously if the spec directory or the sources go
missing.

`tests/integration/test_http2_route_precedence.cpp` pins a second thing this now depends on: the
matcher is first-registered-wins with no preference for a literal segment over a parameter one, so
`/configurations/retrieve` and `/configurations/{configId}` are separated by registration order
alone. They do not collide in today's spec -- the item path defines no POST -- but that is a
property of the YAML, not of the router.

### What this does not fix

The ten services were reachable at the wrong URI; they are now reachable at the right one. Their
downstream status is unchanged, which is to say most still have none (ADR-0324). And the honest
reading of this ADR is that the disclosure discipline in ADR-0322/0323/0324 -- careful about what
was wired and what was not -- was running alongside an unchecked assumption about the thing those
ADRs were most confident about. Volume was the cause: twenty services at six routes each is a lot
of surface to review by eye, and eye review is what was happening.

### The bound on the check

`validate_api_roots.py` covers API root *constants*. It does not cover roots written as inline
string literals, and NEF has one: `/nnef-callback/v1/...`, which it builds notification URIs
under. That one is not a defect -- a notification URI is opaque to the peer that receives it, so
NEF is entitled to choose its own path, and no YAML declares a callback root to conflict with. But
the `nnef-` prefix makes a project-chosen path look 3GPP-declared, and the check would not catch
it if it ever did conflict. Stated here rather than left for the next audit to find.

580/580 plus the two new tests, against a fully current build.

