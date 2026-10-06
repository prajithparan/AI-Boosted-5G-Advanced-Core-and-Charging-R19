## ADR-0387: roaming-interconnect cut over to the consolidated charging DB -- TMF651 agreements in their domain home

**Date:** 2026-09-26. **Status:** accepted (continuing the user-directed cut-over, ADR-0384..0386).

**Decision.** An interconnect agreement is a TMF651 Agreement plus roaming specifics. The TMF651 part
is stored in the domain's single TMF651 home, `subscriber_mgmt.agreement*` (normalized); the roaming
part (partner PLMN, opaque `rateTerms`) in `roaming.interconnect_agreement`, whose `agreement_ref`
is FK'd to the agreement; both rows share ONE server id from `subscriber_mgmt.agreement_id_seq`
(one id space for every TMF651 agreement). TAP3 files live in `roaming.roaming_cdr_file`.
New idempotent `51-agreement-roaming-lossless.sql` fixes what `50-agreement.sql` lost against
`bss_sid::Agreement`: AgreementItem's product / productOffering / termOrCondition **lists** (were one
product + one offering per row, terms hung off the agreement) -> `agreement_item_product`,
`agreement_item_offering`, item-scoped terms; `associatedAgreement` (no home) ->
`agreement_associated`; completionDate as a period; agreementSpecification href/description;
ordinals; and `roaming_cdr_file` gains the **TAP3 payload, format and agreement link** it had no
column for. `rateTerms` not supplied stays absent (stored as JSON null, not `{}`). Malformed
date-times / a TAP3 file naming a non-existent agreement -> 400. Config, compose, both CI jobs ->
`postgres-chf/charging`.

**Rejected.** *Keep the TMF651 agreement as JSONB on the interconnect row* -- a second, unqueryable
agreement home. *Separate id spaces* -- the nested TMF651 id has always equalled the interconnect id.

**Disclosed.** Stores keep one connection + mutex (not a hot path). Old per-service lab rows not
migrated. Remaining on its own DB: the CHF's `chf_rating`.

**Tests.** New `tests/integration/test_roaming_lossless.cpp`: fully populated agreement (every field,
lists >= 2 incl. per-item lists) round-trips exactly via get() and list(); omitted rateTerms stays
omitted; TAP3 bytes round-trip; dangling agreement / bad date -> 400. Existing
`RoamingInterconnectPostgres*` pass. Locally 5/5 (2026-09-26).

