## ADR-0315: AsSessionWithQoS -- an AF's QoS request becomes a real PCF app-session

**Date:** 2026-09-07. **Status:** accepted. **Second** of the 57 AF-facing services (ADR-0302 was
the first).

TS 29.122's AsSessionWithQoS is the classic AF-facing QoS request. Six operations on the same
collection/item shape TrafficInfluence uses, and the same principle decides the design: **a NEF
that records an AF's QoS request and tells nobody has authorised nothing.** PCF installs QoS, via
`Npcf_PolicyAuthorization` app-sessions -- an endpoint this project already implements, including
the real `POST .../app-sessions/{id}/delete` (checked, not assumed).

### What is brokered, and the two properties that make it real

- **Create** posts a genuine `AppSessionContext` to PCF and records the app-session id from the
  `Location` header.
- **Delete** tears that app-session down. Without it an AF could delete its subscription while the
  QoS it asked for stayed installed -- an authorisation outliving the request that made it. The
  app-session id is read BEFORE the subscription is removed, because removal erases it.
- **PCF is told to notify NEF, not the AF.** The AF's own `notificationDestination` is where NEF
  sends events; pointing PCF directly at the AF would route core-network notifications around the
  exposure function, which is the one thing NEF exists to prevent.

### Deliberately not mapped, in the code as well as here

- `flowInfo`/`ethFlowInfo` and `qosReference` belong inside PCF's `medComponents` ->
  `medSubComps`, a nested structure with real semantics (media type, flow direction). Half-wiring
  them would produce a session that appears authorised for specific flows when it is not. A session
  with no media component is honest: PCF authorises the session, not the flows.
- `gpsi`/`extGroupId` identify the AF's target; `AppSessionContextReqData` identifies by UE
  address, and translating one to the other needs a UDM lookup this route does not perform.
- **PUT does not update the existing app-session.** Modifying one needs
  `AppSessionContextUpdateData` via PATCH -- a different translation, not an approximation of this
  one. A replaced subscription keeps its original authorisation until deleted.

### Three assumptions checked rather than trusted

PCF's delete path (`POST .../delete` -- correct); PCF's port (I wrote 7782; it is **7783**); and
`self_base_url`, which I used as though it were a variable in `main()` -- it exists in
`config/nef.json` but had never been read, because nothing before now needed to tell another NF
where to find this one.

### The build failure this slice sat behind, since it cost hours

A single legitimate OOM kill left `build/.ninja_deps` (3 MB) and `.ninja_log` **truncated**. Every
subsequent invocation then printed `ninja: warning: premature end of file; recovering`, discarded
its dependency knowledge, treated the whole tree as dirty, and attempted a maximal rebuild -- long
enough to be killed again, re-truncating the files. A self-sustaining loop, whose symptom was
"progress never accumulates: still 218 pending steps" and whose cause was visible in the FIRST LINE
of every build log I had been greping past.

Recovering it: delete BOTH state files (I removed only `.ninja_log` first and wrongly concluded the
fix had landed), then build in staged targets so each invocation completes and its progress
persists. Full build then reached zero pending steps, and 580/580 tests pass.

Wrong explanations I offered along the way, recorded because the pattern matters more than the
incident: that CHF's `main.cpp` had become expensive (measured 2.55 GB -- unremarkable); that NEF's
compile had spiked (it was failing fast on a missing include I had not yet read); and that kills
were "near the link step" (inferred from `ninja -n` output that says no such thing). The reliable
order is: read the build output first -- all of it, including the first line -- and measure only
when the output does not explain the failure.

