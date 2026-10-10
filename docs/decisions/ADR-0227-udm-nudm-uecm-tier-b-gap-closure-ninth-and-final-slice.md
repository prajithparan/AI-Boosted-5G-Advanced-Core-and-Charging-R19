## ADR-0227: UDM `Nudm_UECM` Tier-B gap-closure -- ninth and final slice (`Trigger P-CSCF
Restoration`, `GetLocationInfo`, `authTrigger`)

### Context

Closes `Nudm_UECM`'s entire remaining Tier-B backlog: the three independent single ops left after
ADR-0215 through ADR-0221 closed every CRUD/composition group. Confirmed directly from
`specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`:

- **`Trigger P-CSCF Restoration`** (`POST /restore-pcscf`, real, unusual `operationId` literally
  containing spaces in the source YAML, not normalized here): no `ueId` path param -- the target
  UE is identified by `supi` in the `TriggerRequest` body. Real spec intent (TS 23.380): UDM
  relays this trigger onward to the UE's serving AMF, which carries out the actual P-CSCF
  restoration procedure.
- **`GetLocationInfo`** (`GET /{ueId}/registrations/location`): real, and genuinely different from
  the other two -- a complete local composition, no external relay needed.
- **`authTrigger`** (`GET /{ueId}/registrations/trigger-auth`, a real GET with a request body, an
  unusual but genuine OpenAPI pattern already seen elsewhere in this project): real spec intent
  (TS 33.501) is UDM relaying a primary re-authentication trigger onward to AUSF/the serving AMF.

### Implementation

- **`GetLocationInfo`**: `RegistrationLocationInfo`'s own `amfInstanceId`/`guami`/`vgmlcAddress`
  fields already exist directly on the stored `Amf3GppAccessRegistration`/
  `AmfNon3GppAccessRegistration` records (ADR-0215/ADR-0216) -- no fabrication. `accessTypeList` is
  genuinely determined by which of the two registration groups a UE has (3GPP vs non-3GPP access is
  exactly what distinguishes them in the real spec). Real, disclosed narrowing found and fixed
  during implementation: `Guami.plmnId` is the real, distinct `PlmnIdNid` type (mcc/mnc + optional
  SNPN `nid`), while `RegistrationLocationInfo.plmnId` expects the plain `PlmnId_CommonData`
  (mcc/mnc only) -- genuinely different real schemas, not interchangeable; an SNPN's own `nid` is
  dropped in the conversion since the target schema has no field for it. A `ueId` with neither
  registration is a real `404`.
- **`Trigger P-CSCF Restoration`/`authTrigger`**: real, disclosed simplification, same class as
  ADR-0207's own SendSMS disclosure ("no real onward IP-SM-GW/SMSF relay wired -- the real ack is
  NEF-level acceptance only"): this project has no real onward relay to the serving AMF/AUSF that
  would carry out the actual restoration/re-authentication procedure -- accepting the request and
  confirming the identified UE (`supi`/`ueId`) has a known AMF registration is genuine work; the
  relay itself is not wired. A `supi`/`ueId` with no known registration at all is a real `404`;
  otherwise a real `204`, matching each operation's own real spec-documented success code.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_227.cpp`, 1 test
(`TriggerPcscfRestorationGetLocationInfoAuthTrigger`): real `404` for all three operations before
any AMF registration exists; after seeding a real `amf-3gpp-access` registration, `GetLocationInfo`
returns a real `200` with the exact seeded `amfInstanceId`/`plmnId`/`3GPP_ACCESS` access type, and
both trigger operations return real `204`.

Passes.

### Testing

Full reconfigure + rebuild clean. The new test passes individually. Full suite re-run clean (see
commit for exact pass count). No strays before or after any run (`ps aux` checked explicitly).

### What this ADR does NOT include

The real onward relay to AMF (P-CSCF restoration)/AUSF (re-authentication trigger) for the two
trigger operations -- both accept and validate only. This closes `Nudm_UECM`'s entire Tier-B
backlog. `Nudm_SDM` (~33 ops, needs the UDM->UDR proxy pattern confirmed first) and `Nudm_PP` (11
ops, already flagged deferred in ADR-0082) are UDM's only remaining Tier-B items.

