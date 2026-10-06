## ADR-0286: the N28/Sy chain reaches SMF -- the standing directive's missing half

**Date:** 2026-09-05
**Status:** Accepted

### The gap, and why a green test hid it

A standing user directive (2026-08-16) required N28/Sy to be real end to end with **PCF and SMF**,
plus a GUI-creatable data model for `policyCounterIds`. `PcfChfN28Integration` passed, so the chain
looked finished. A status re-check on 2026-09-05 found it was not:

- **SMF had no `policyCounterId` code at all** -- `grep` over `nfs/smf/src/main.cpp` returned
  nothing.
- SMF had been **advertising** `notificationUri = .../sm-contexts/{ref}/pcf-notify` to PCF since
  ADR-0038, and **nothing served that path**. PCF's own file header records the mirror image:
  "neither AMF nor SMF implements the receiver."

So a policy change PCF decided -- including one driven by a CHF spending-limit status change over
N28 -- had nowhere to land. The PCF↔CHF test being green is exactly what made that invisible.

### The rule is data, because the spec refuses to define it

PCF's header already explained why the last hop was never built: TS 29.594 makes
`PolicyCounterInfo.currentStatus` a **free-form string the spec never enumerates**, so any
"status X ⇒ policy Y" rule written in C++ would be invented.

So the mapping is **configuration**: `policy_counter_actions` in `config/pcf.json`, each entry
naming a `policyCounterId`, a `currentStatus`, and the `SmPolicyDecision` to push when they match.
The operator owns the rule; PCF owns the mechanics; a GUI later edits that same JSON without any
mechanism change. That is this project's own **P7** ("policy is data, never code") applied to the
one place it was still deferred for lack of a rule to encode -- and it is the "GUI-creatable data
model" half of the directive.

### What was built

- **SMF**: the `pcf-notify` receiver it had been advertising for months. Records the pushed
  decision under `pcfPolicyUpdate` on the SM context (kept separate from the decision captured at
  establishment, so an operator can see both) and counts it in `smf_pcf_policy_updates_total`.
- **PCF**: on a spending-limit status change, looks up the operator's configured action and posts a
  real `SmPolicyNotification` to the `notificationUri` SMF supplied. Counted in
  `pcf_sm_policy_updates_pushed_total`.

### Four failures, each a real fact about the system

Recorded because they are what the test now protects:

1. `404 No route` -- TS 29.594 delivers to `{notifUri}/notify`; PCF serves
   `.../spending-limit-notify/notify` while handing CHF the URI without that suffix.
2. `404 No tracked subscription for SM policy 1` -- PCF allocates `smpolicy-N`, not `N`.
3. `404 No tracked subscription for smpolicy-1` -- the real precondition: PCF only subscribes to
   CHF when the subscriber's **UDR** policy data carries `subscSpendingLimits: true` with at least
   one counter id. No UDR, no subscription, nothing for a status change to attach to.
4. The final assertion could not read `pcfPolicyUpdate` back, because `/retrieve` answers with a
   spec-shaped `SmContextRetrievedData`. **The tempting fix was to add the field to that response**
   -- putting a non-spec key in a spec response to make a test pass. Rejected; the assertion reads
   the counters both NFs export instead, the same reasoning `test_n28_spending_limit.cpp` already
   applies to PCF's internal state.

### Still open on the directive

**The GUI.** Phase 7 has not started and its stack decision (React + JSON Forms vs Dear ImGui) is
still open with the user. The data model it would edit now exists and is live.

**Enforcement.** SMF records the pushed decision; translating a specific decision into a PFCP
re-authorisation is a separate increment with its own N4 work, named here rather than half-built.

---

