## ADR-0317: NEF actually delivers notifications

**Date:** 2026-09-07. **Status:** accepted. **Closes:** a gap ADR-0302, ADR-0315 and ADR-0316 each
disclosed separately.

All three AF-facing slices accepted `notificationDestination`, stored it, and delivered **nothing**.
UDM and PCF dutifully called NEF's callback URIs and the reports stopped there -- so an AF that
subscribed correctly, through a working API, was simply never told anything. Three ADRs each
recorded it as "still absent"; it is one piece of work, not three.

### Correlation by URI, not by state

The callback NEF hands to UDM carries the AF id and subscription id **in its own path**
(`/nnef-callback/v1/monitoring-notify/{afId}/{subscriptionId}`), and UDM calls back exactly the URI
it was given. So an inbound report identifies its own subscription with nothing to keep in sync --
no correlation-id table that could drift from the subscriptions it describes, and no lookup that
could go stale.

### Four decisions inside it

- **Not bearer-checked.** The caller is UDM over mTLS, on a URI NEF generated and handed out
  itself. Requiring an OAuth2 token would mean UDM's event-exposure path holding an NEF-scoped
  token it has no reason to have.
- **A deleted subscription answers 404**, not 204. That tells UDM the callback is dead rather than
  silently accepting reports for nobody -- the AF is gone, and UDM should stop.
- **The notification is REBUILT in the AF's own API shape**, not forwarded verbatim. UDM sends a
  `Nudm_EE` report; the AF subscribed through TS 29.122 and is entitled to that API. `subscription`
  points at the AF's own resource, not UDM's.
- **The raw UDM report is carried under `_nudmEeReport` rather than re-modelled.** This build does
  not map every `Nudm_EE` report field onto TS 29.122's `MonitoringEventReport`, and inventing that
  mapping would put fabricated values in front of an AF. The AF gets everything NEF can vouch for,
  plus the original, clearly labelled.

That last point is the honest half of this ADR: delivery is real, the payload translation is
partial and says so.

### Scope

Monitoring notifications only. The PCF-facing QoS callback (`/qos-notify`, ADR-0315) is wired at
PCF but has no NEF route yet, so QoS events still stop at the door -- the same gap this ADR closes
for monitoring, one service later. TrafficInfluence has no notification path at all yet.

580/580 against a fully current build.

