## ADR-0257: SMF small-data MO ops closed; `Update SOR Info` re-examined and correctly re-deferred

### What was built

`SendMoData` and `TransferMoData` (`TS29502_Nsmf_PDUSession.yaml`) -- the last two paths ADR-0252's
whole-project audit classified as **genuinely absent** rather than deliberately deferred:

| Path | Method | Body |
|---|---|---|
| `/sm-contexts/{smContextRef}/send-mo-data` | POST | `multipart/related`, `SendMoDataReqData` + binary |
| `/pdu-sessions/{pduSessionRef}/transfer-mo-data` | POST | `multipart/related`, `TransferMoDataReqData` + binary |

Both were deferred at SMF's first turn (ADR-0032) as "peripheral to Phase 2" -- a **scoping call,
not a spec blocker** -- and both are the mobile-originated mirror of `Nsmf_NIDD`'s own
mobile-terminated `Deliver`, already implemented here (ADR-0201). They reuse that route's exact
pattern: `parse_multipart_json_body`, existence check against the SM context store, 204.

Both are `multipart/related`-**only** in the real spec (no `application/json` alternative exists for
either), and each defines exactly one success response: 204, no body. Read per path, not assumed.

**Disclosed, same class as `Deliver`'s own**: the binary `moData` part is validated as present by
the multipart codec, but no onward path to a DN or to NEF's `Nnef_SMContext` exists, so the
operation accepts and validates -- it does not pretend to deliver. `moExpDataCounter`/`ueLocation`
are accepted and not acted on. `pduSessionRef` resolves against the `smContextRef` id space, the
same disclosed identifier simplification `Deliver` makes.

### `Update SOR Info`: ADR-0234's two stated blockers were both wrong

This was the third "genuinely absent" path. It was investigated properly rather than implemented on
ADR-0234's stated reasoning, and **that reasoning does not survive contact with the current tree**:

- **"No real per-UE `CounterSoR` state machine (TS 33.501 6.14.2.3) exists anywhere in this
  build."** It does -- in **AUSF**, which is architecturally where it belongs because AUSF owns
  KAUSF. `kausf_store.use_counter()` implements it, including real wrap-around suspension citing
  that exact clause. ADR-0234 looked for it in UDM and concluded it did not exist at all.
- **"Real steering-of-roaming list content is missing."** True, but **not blocking**.
  `steeringContainer` is optional in both `SorInfo` and in AUSF's own `Nausf_SoRProtection`
  request, `SorInfo` requires only `ackInd` + `provisioningTime`, and TS 33.501 Annex A.17 makes
  the Steering Info List an optional P2 of the MAC computation.

So the operation looked closeable: UDM relays `POST /{supi}/ue-sor` to AUSF, which already returns
a real `SorSecurityInfo{sorMacIausf, counterSor}` from a real KAUSF. UDM already has the
per-target-NF OAuth2 client pattern to do it.

### The real blocker, which ADR-0234 did not identify

**AUSF requires a `sorHeader`** on that request, and refuses rather than fabricating one -- a
correct, already-disclosed limit in its own file: AUSF-side construction of the SOR header is
**TS 24.501 clause 9.11.3.51**, a NAS-layer bit encoding.

**TS 24.501 is not in `specs/`, and no SOR transparent container exists anywhere in this project's
NAS codec** (checked, not assumed). UDM constructing that header would be inventing a wire
encoding -- the one thing this project never does.

`Update SOR Info` therefore stays deferred, on **missing spec material**, the same class as
ADR-0104's AUSF ProSe deferral. It moves from "genuinely absent gap" to **disclosed deferral with
an accurate reason**. Acquiring TS 24.501 would unblock it, and nothing else would.

The stale reasoning is corrected in `nfs/udm/src/main.cpp` rather than left to justify the right
decision with wrong facts -- the same ADR-0250/0251 discipline ADR-0256 applied to `GetSSAuData`.
Note one half of that comment was **not** changed because it is still true: there is genuinely no
AUSF endpoint consuming a UE SoR ack, so the ack routes' non-relay disclosure stands.

### ADR-0252's audit is now fully discharged

All 328 YAML paths across 16 NFs are implemented or carry an explicit, reasoned deferral. **Zero
undisclosed gaps.** The remaining unrouted paths are: NRF 5, AUSF 1, UDR 1, UDM 1 -- every one a
recorded decision, three of them with reasons corrected in the last two ADRs.

### Verification

Both new paths confirmed by exact-literal `grep -F` against `build/nfs/smf/smf`. Build clean,
clang-format clean.

