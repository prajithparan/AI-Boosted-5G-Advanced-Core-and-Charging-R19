## ADR-0296: cancelLocation -- MAP's deregistration half, and a mapping I refused to invent

**Date:** 2026-09-05. **Status:** accepted. **Follows:** ADR-0293.

ADR-0293 wired UDM's MAP client and closed with an explicit list of what "MAP is done" must not be
read to mean. Top of that list: "`Nudm_UECM` deregistration does not send `cancelLocation`, the
natural MAP counterpart." A registration that tells a VLR about a subscriber and a deregistration
that never tells it to stop leaves real VLRs accumulating subscribers they no longer serve. That
is the half closed here.

### The operation

`libs/map-core` gains `cancelLocation` (opcode 3, already declared). Structure taken from
TS 29.002 and cross-checked field-for-field against jss7's own `CancelLocationRequest` /
`CancelLocationRequestImpl` -- arms-length reference, read and cited, never linked, the same
arrangement ADR-0059 established (jss7 is AGPL-3.0; this project is Apache-2.0).

Two encoding facts differ from `insertSubscriberData`, and both are the kind of mistake that is
silent on the wire:

1. **The argument is a CONTEXT-SPECIFIC [3] CONSTRUCTED tag, not a UNIVERSAL SEQUENCE.** jss7
   `TAG_cancelLocationRequest = 3`, `getTagClass()` → `CLASS_CONTEXT_SPECIFIC`, `getIsPrimitive()`
   → false for v3. The existing operation in this codec sets exactly the opposite precedent, so a
   test pins the first byte at `0xA3` and a second test rejects a `0x30`-wrapped argument.
2. **`identity` is an untagged CHOICE, identified by position.** Its two arms are told apart by
   their own universal tags (IMSI a primitive OCTET STRING, IMSI-WithLMSI a constructed SEQUENCE),
   so jss7 decodes it positionally and so does this. Only the `imsi` arm is modeled; an
   `imsi-WithLMSI` is reported as a decode failure rather than read as an IMSI.

The application context is `locationCancellationContext-v3`, **not** the `subscriberDataMngtContext`
the existing operation uses. jss7's `MAPApplicationContextName` gives the arc directly
(`locationCancellationContext(2)`) and `MAPApplicationContext` builds every AC OID from
`{0, 4, 0, 0, 1, 0, <ac-Id>, <version>}` -- a template that independently reproduces the existing
`subscriberDataMngtContext(16)` constant, which is what makes it trustworthy rather than a pattern
guessed from one example.

### The mapping I did not invent

The obvious next move was a table from TS 29.503's seven `deregReason` values onto TS 29.002's
`cancellationType`. I wrote one, then removed it. **No 3GPP specification defines that mapping** --
it is interworking policy, and getting it wrong puts a wrong fact on the wire silently: telling a
VLR `updateProcedure` ("the subscriber moved") when it did not, or the reverse.

What ships instead: `cancellationType` is OPTIONAL in the real ASN.1, and UDM sends it for exactly
one value -- `SUBSCRIPTION_WITHDRAWN` → `subscriptionWithdraw` -- where the correspondence is a
name identity rather than a judgement. For the other six it is omitted, which is genuinely valid
and claims nothing. Both paths have a test.

This is the same rule that stopped `GetSSAuData` and the `smsRouter` composition from being
guessed at: an omitted optional field is honest, an invented value is not.

### Evidence

The integration test now serves **two** dialogues on two associations -- the client opens a fresh
one per operation -- and asserts a real `insertSubscriberData` after `PUT .../amf-3gpp-access` and
a real `cancelLocation` after `POST .../amf-3gpp-access/dereg-amf`, each answered with a real AARE
+ TC-END so the client's response path executes. Both are visible in UDM's own log:
`insertSubscriberData accepted by VLR` then `cancelLocation accepted by VLR`. Six codec unit tests
cover the round trip, the omitted-optional path, the `[3]` wrapper, and both rejection cases.

`send_map_operation` was factored out of the existing client rather than cloned: the association,
handshake, SCCP addressing, TC-Begin framing, AARE check and component interpretation were already
identical, so only the opcode, application context, parameter and result decoder vary.

### Still not done

`libs/map-core` now encodes two of TS 29.002's operations. `updateLocation`,
`sendAuthenticationInfo`, `purgeMS`, `deleteSubscriberData` and `checkIMEI` have declared opcodes
and no codec. On the CAP side, `cap_server` still handles only
`InitialDP → RequestReportBCSMEvent+ApplyCharging`; `EventReportBCSM` and `ApplyChargingReport`
are unimplemented, so a call's **end** is never reported back and no final charging is applied.
That CAP gap is the larger of the two remaining and is the next real increment.

