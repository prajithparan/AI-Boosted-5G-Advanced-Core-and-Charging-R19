## ADR-0322: three AF provisioning services in one batch

**Date:** 2026-09-08. **Status:** Closed (work pushed; last citing commit f1fedd2 on origin/main, 2026-10-06). accepted. Sixth, seventh and eighth of the 57 AF-facing services.

IPTVConfiguration, LpiParameterProvision and ACSParameterProvision -- 18 operations across three
TS 29.522 services, done as one increment rather than three, because they are genuinely the same
shape and building them separately would have been three build/test cycles for no additional
insight.

### One shared store, and why the earlier ones stay separate

These three are per-AF keyed documents with identical lifecycles and **no service-specific state**,
so they share `AfDocumentStore`. The four earlier AF stores stay separate deliberately:
`AfQosSubStore` carries a PCF app-session id, `AfMonitoringSubStore` carries a UDM
ee-subscription pair. Collapsing those into a generic store would mean pushing service-specific
state into a shape that has nowhere to put it. The shared class here expresses a real sameness
rather than abstracting over things that differ.

### Only one of the three has a downstream, and that is disclosed

`application-data/iptvConfigData` is a real UDR resource this project already serves, so IPTV
configurations are provisioned there and removed on DELETE.

**LPI (Local Positioning Information) and ACS (Auto-Configuration Server) parameters have no UDR
counterpart in this project's data model.** They are accepted, validated against their real
generated DTOs, and stored at NEF -- and nothing downstream applies them. The API is real and
conformant; the effect is not there yet. Said plainly, because "18 operations added" would
otherwise read as three working features rather than one working and two staged.

### Build note

First `-j3` build of the session, no OOM kill -- ADR-0313's codegen split plus stopping the Doris
container during builds. The earlier `-j1` caution was a hangover from the corrupted-ninja-state
period (ADR-0315) rather than a real constraint on this hardware.

580/580 against a fully current build.

