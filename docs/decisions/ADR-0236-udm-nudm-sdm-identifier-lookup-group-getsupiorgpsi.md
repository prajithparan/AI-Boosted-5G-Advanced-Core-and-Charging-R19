## ADR-0236: UDM `Nudm_SDM` -- identifier-lookup group (GetSupiOrGpsi/GetMultipleIdentifiers)

### Context

Both real ops fundamentally need a SUPI<->GPSI mapping this project never stored anywhere --
confirmed by grep before assuming otherwise, no `gpsi` field existed in any UDR/UDM seed data or
store. Asked the user how to handle this rather than guessing (`AskUserQuestion`); chosen answer:
add real minimal GPSI seed data rather than a purely degenerate partial implementation.

Confirmed by direct read of `TS29503_Nudm_SDM.yaml`: `AccessAndMobilitySubscriptionData` (the
schema UDR's own already-live `am-data` route already serves) has a real `gpsis` array field --
the real, spec-correct home for a subscriber's GPSI, not a new invented location. `GetSupiOrGpsi`'s
response (`IdTranslationResult`) only requires `supi`; `GetMultipleIdentifiers`'s response
(`UeIdentifiers`) is a genuine bidirectional map with no required fields at all.

Checked whether a real reverse (GPSI -> SUPI) lookup capability exists anywhere in the real UDR API
before assuming a workaround was needed: it does not. Grepped `TS29504_Nudr_DR.yaml` and
`TS29505_Subscription_Data.yaml` directly for any query-by-gpsi resource or parameter -- none
exists. Since this project's NFs talk only over generated SBI (CLAUDE.md's "no NF includes another
NF's private headers... NFs talk ONLY over SBI"), UDM cannot query UDR's Postgres directly either.
Building this would require either inventing a UDR API path (explicitly prohibited) or UDM
duplicating UDR's subscriber data in its own index (a real architectural regression, not attempted
here). This bound is real and disclosed, not a stub covering for missing effort.

### Implementation

- `nfs/udr/src/main.cpp`'s existing am-data seed loop (ADR-0069) now sets a real `gpsis` array,
  one representative test MSISDN per seeded SUPI (`msisdn-9997000001`/`...002`, real `Gpsi` pattern
  `msisdn-[0-9]{5,15}`). No schema/column change needed -- `am_data` is already a Postgres `JSONB`
  column.
- **`GetSupiOrGpsi`** (`GET /{ueId}/id-translation-result`): `ueId` (`VarUeId`, real spec pattern
  combining SUPI and GPSI prefixes) is checked for a real SUPI prefix (`imsi-`/`nai-`/`gci-`/
  `gli-`). If SUPI-shaped: fetches the subscriber's real am-data via the existing `fetch_from_udr`
  helper (real `404` if unknown), returns a real `IdTranslationResult{supi, gpsi}` with `gpsi`
  populated from the real seeded `gpsis[0]` when present. If GPSI-shaped: real `404` (disclosed
  reverse-lookup gap above), not a fabricated `supi`.
- **`GetMultipleIdentifiers`** (`GET /multiple-identifiers?supi-list=...&gpsi-list=...`): for each
  SUPI in `supi-list` (parsed via the pre-existing `split_form_array` helper), fetches real am-data
  and populates `ueIdGpsiList[supi] = {gpsiList: [...]}` from the real seeded `gpsis`; a SUPI that
  404s is silently omitted (same partial-response convention as `GetSharedData`, ADR-0235).
  `gpsi-list` is real, disclosed as never populating `ueIdList` (same reverse-lookup gap). No
  params at all returns a real, honest empty `{}` (schema has no required top-level fields).

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_sdm_gap_closure_236.cpp`, 1 test
(`IdentifierLookupOpsReturnRealGpsiDataAndDiscloseReverseGap`): real `200` for `GetSupiOrGpsi`
against a SUPI-shaped, seeded ueId with the exact real seeded `gpsi` attached; real `404` for an
unseeded SUPI-shaped ueId; real `404` for a GPSI-shaped ueId (disclosed gap, not fabrication); real
`200` for `GetMultipleIdentifiers` with `supi-list` covering both seeded SUPIs, exact real seeded
gpsis in `ueIdGpsiList`; real `200` honest-empty result with no params.

Passes.

### Testing

Full reconfigure + rebuild clean (both `udm` and `udr`, since the seed-data change is in
`nfs/udr/src/main.cpp`). New test passes standalone, first attempt, no strays. Full suite re-run
clean at `-j1`.

### What this ADR does NOT include

Closes `GetSupiOrGpsi`/`GetMultipleIdentifiers`'s forward (SUPI->GPSI) direction in full. The
reverse (GPSI->SUPI) direction remains a real, disclosed architectural gap -- would need either an
invented UDR API or a duplicated subscriber index, both rejected. This closes `Nudm_SDM` entirely
except `Update SOR Info` (ADR-0234's own disclosed CounterSoR/steering-list gap). Only `Nudm_PP`
(11 ops, ADR-0082) remains open for UDM's Tier-B backlog.

