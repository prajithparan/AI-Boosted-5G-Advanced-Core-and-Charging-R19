## ADR-0320: SMF finally reads the traffic-influence data NEF has been writing

**Date:** 2026-09-08. **Status:** accepted.

Asked to add TrafficInfluence notifications, I checked what would trigger one before building a
route to receive it. Nothing would: **no code in this project reads `influenceData`**, and nothing
handles `upPathChgNotifUri`. SMF had no UDR client at all -- it talks to PCF, NSACF, AMF, CHF and
NRF only.

So NEF has been faithfully writing `TrafficInfluData` into UDR since ADR-0302, and no NF has ever
looked at it. **An influence rule that no SMF consults does not influence anything.** That is the
same shape of gap ADR-0293 found when UDM's MAP client had no caller, and ADR-0311 found when
nothing could read a CDR back -- a component doing its half correctly, with the other half absent.

A NEF notification route would have been dead code sitting on top of that.

### What this adds

SMF gains a UDR client and, at PDU session establishment, fetches the influence records that apply
to the session's DNN and S-NSSAI.

**Matching is done in SMF, not by query filter, deliberately.** UDR's influenceData collection GET
honours only `influence-Ids` (ADR-0253 discloses this; ADR-0302 fixed that one filter after finding
it never matched anything). Asking UDR to filter by dnn/snssai would silently return the
*unfiltered* set -- so SMF fetches and matches itself rather than trusting a filter documented not
to work. A record with neither `dnn` nor `snssai` applies to every session, which is what an
unconstrained rule means.

### What it deliberately does NOT do

Matching records are logged and their DNAI route targets recorded. **Actually steering traffic to a
DNAI requires selecting a different UPF for the session, and this project has one UPF and no
re-selection.** Acting on the routing decision is a real, separate capability this does not pretend
to have.

### Why TrafficInfluence notifications still cannot be built

`upPathChgNotifUri` fires when the user-plane path CHANGES. With one UPF and no DNAI-based
selection, there is no path change to detect -- so a notification route would never fire. That is
not a NEF gap, and building the receiving end first would create the appearance of a working
feature. The prerequisite is DNAI-based UPF selection in SMF/UPF; when that exists, the
notification path is small and NEF's side follows the mechanism ADR-0317/0318 already established.

Stated plainly because "TrafficInfluence notifications" sounds like NEF work and is not.

580/580 against a fully current build.

