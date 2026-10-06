## ADR-0447: the `charging_data_ref` correlator, populated -- increment 2, part 1

**Date:** 2026-10-02. **Status:** done (the correlator); the reconciliation sweep that consumes it
is scoped below, not yet built.

**Real, previously-undiscovered finding while starting this increment.**
`balance_mgmt.reserve_balance.charging_data_ref` has existed in the schema, with its own index
(`idx_reserve_ref`), since the table's original definition in `deploy/db/charging/40-balance.sql`
-- `-- the CDR session this reserves against`, right there in the column comment. Checked directly:
zero references to it anywhere in `bss/balance-management/src/store.cpp` or any other C++ file in
this repository. The column and its index have been sitting unused since whenever that table was
first created -- found while verifying ADR-0445's claim that no such correlator exists, not
assumed. This means increment 2's "add a column" step from ADR-0445 is actually "wire up a column
that was already there," which is simpler and lower-risk than a schema migration would have been.

**Design: an HTTP header, not a TMF654 body field.** `bss_sid::ReserveBalance`'s real wire shape
(confirmed against the real swagger when `libs/bss-sid/balance.hpp` was written) has no
characteristics/extension-point array the way `ProductOfferingPrice.prodSpecCharValueUse` does --
there is no spec-conformant place in the JSON body to carry an implementation-specific correlation
key. `description` already contains the ref embedded in free text
(`"Nchf_ConvergedCharging_" + operation + " " + ref`, `charging_engine.cpp`), but relying on string
parsing for a reconciliation key is fragile and was not the design chosen. Instead: a new,
clearly-non-3GPP HTTP header, `x-chf-charging-data-ref` (lowercase `x-` prefix, visually distinct
from the real `3gpp-Sbi-*` namespace `libs/sbi-core/include/sbi_core/sbi_headers.hpp` already
reserves for actual TS 29.500 headers, so nobody mistakes this for a spec-defined one). CHF sets it
on both the reserve and the unreserve call (both go through `POST /reserveBalance`, same function,
`reserve_subscriber_balance`/`finalize_subscriber_balance` in `charging_engine.cpp`); the permanent
debit (`POST /adjustBalance`) does not carry one, because `adjust_balance` has no such column and a
permanent debit is not what the recovery query below needs to find. `bss/balance-management/src/
main.cpp`'s route handler reads the header and passes it to `BalanceStore::reserve()` as a new
`std::optional<std::string>` parameter (nullable: a reserve/unreserve call from anywhere other than
CHF's own real charging path -- a manual operator action, a future consumer -- has no session to
tie to, a real, valid state, not an error). The TMF654 response body is unchanged; this is a
server-side-only column, never serialized.

**Verification.** A new test, `BalanceLossless.ChargingDataRefIsStoredAndSumsPerSession`
(`tests/integration/test_balance_lossless.cpp`), proves two reserves under the same session ref sum
correctly (`SELECT SUM(amount_value) WHERE charging_data_ref = $1`), a different session's rows
don't bleed into that sum, and a reserve with no ref stores real `NULL` rather than a colliding
empty string. All 7 `BalanceLossless.*` tests pass (no regressions). End-to-end, live proof against
a real CHF process (`CapScopedCharging`/`ChaosCharging`, both pass): `SELECT id, bucket_id,
charging_data_ref, amount_value FROM balance_mgmt.reserve_balance` after a real test run shows a
real row -- `imsi-999700000424242 | chg-31843 | 10` -- `chg-31843` being the actual
`ChargingDataRef` CHF generated for that real session, correctly carried all the way from CHF's
HTTP call through balance-management's header read into the previously-dead column.

**Not yet done: the reconciliation sweep itself.** ADR-0445's decision #2 was "the unreserve/debit
failure window is addressed instead by a reconciliation mechanism (detect a bucket whose
`reserved_value_amount` is non-zero with no live `ChargingDataRef` holding it... and repair it)."
The correlator above is the prerequisite data; the sweep that reads it is a separate, larger piece
of work with its own real design question this ADR does not answer unilaterally:

- **Detection only, not auto-repair, in the first pass.** Matches this project's own established
  pattern for exactly this class of problem -- P12's CDR-sequence-gap alarm (ADR-0282) detects and
  alarms, it does not silently self-heal a billing anomaly. Automatically moving money without a
  human in the loop is the kind of action `docs/AI_AGENTS_AND_MODELS.md`'s own PII/write-governance
  mandate ("any write/config action requires explicit human approval") argues against by the same
  logic, even though this isn't an AI agent -- an automated process correcting its own financial
  records unsupervised is the same risk shape.
- **Open question, not decided here: the staleness threshold.** A bucket with `reserved_value_amount
  > 0` and a `charging_data_ref` is not automatically "stuck" -- a real, long-running session (a
  VoLTE call, a long PDU session) legitimately holds a reservation for a while. Flagging it as
  suspect needs a "how long is too long" threshold, and that is an operational/business policy
  choice this ADR will ask about rather than invent, the same way money scale and fail-open/closed
  were asked about rather than guessed.
- **Where it runs:** the natural home is balance-management itself (it already owns both tables
  needed -- `bucket` and `reserve_balance` -- in one database, no cross-service query needed), as a
  periodic internal sweep analogous to CHF's own archive-sweep pattern (ADR-0283), off by default
  until configured, same convention. No new HTTP endpoint is implied or planned -- this is an
  in-process job, not a new TMF654 (or non-TMF654) API surface, so it does not raise the
  invented-API-path risk a new endpoint would.

