## ADR-0332: two of ADR-0331's tools called routes that do not exist

**Date:** 2026-09-10. **Status:** accepted. Correction to ADR-0331, found the same day.

ADR-0331 shipped five tools and singled `explain_charge` out as *"the tool that makes a customer
agent honest"*. **It could not have worked.** It called
`GET /nchf-convergedcharging/v3/rating-decisions/{ref}`, and so did `get_recent_charges` with a
`/cdrs` query — **neither route exists in CHF**. Both would have failed at runtime with a
transport error or a 404.

Found by checking each tool's backend against CHF's actual routes rather than trusting the URLs I
had written. The audit, scoping and refusal tests all passed regardless, because they exercise the
governance layer and never reach a backend — a green suite that says nothing about whether the
tools return data.

### The fix is not to add the routes

The obvious repair — add those paths to CHF — would mean **inventing 3GPP operations**. TS 32.291
defines no "read the rating decision behind a charge" and no CDR query operation, so publishing
either under `/nchf-convergedcharging/v3/` would fabricate a spec path. That is precisely ADR-0325.

So the MCP server reads these two **project-owned** stores directly, read-only, and invents no API
at all. `RatingDecisionStore` gained `find_by_charging_data_ref`, keyed on
`input_snapshot->>'chargingDataRef'` — because that is where the reference actually lives;
`usage_record_id` was reserved for a UsageRecord table that does not exist, so keying on it would
have silently found nothing.

It reuses **CHF's own store implementations** rather than re-implementing the queries, so an MCP
read cannot drift from the tested one. Disclosed coupling: this is a *tool* reading project
stores, not an NF bypassing SBI — `CLAUDE.md`'s "NFs talk only over SBI" rule governs NFs.

### An empty result is an error, not an empty success

`explain_charge` returns an error when no decision is recorded, never an empty object. *"No
decision was recorded for this reference"* and *"this charge had no reason"* are different
statements, and an agent handed `{}` is free to narrate the second. This is the motto as a code
rule: a tool that cannot ground an answer must refuse, so the layer above has nothing to invent
from.

### Verified against real data this time

Seeded a rating decision and drove the server: `explain_charge` returned the tariff, version, rule
fired **and the AI advisory** (`model: quota_sizing, version 3, boundApplied: upper_2x`) — an
AI-influenced charge is visibly AI-influenced rather than indistinguishable from a deterministic
one. An unknown reference returned an error. A pinned `customer-agent` asking for a *different*
subscriber was refused and audited as `denied_policy`.

`get_recent_charges` also now withholds `serving_plmn` and records it as withheld: it is
location-adjacent (it says which country a subscriber was in) and a "what did I spend" question
never needs it.

