## ADR-0448: real Diameter RAR/RAA + its operator trigger -- increment 2, part 2

User-directed, mid-session: asked, while the above sweep's staleness-threshold question was still
open, "what about RAR, it should be implemented" -- redirecting this increment from the sweep's own
open policy question onto a concrete, spec-defined mechanism instead. This ADR covers that
mechanism: real Diameter Re-Auth-Request/Answer (RAR/RAA), plus the one thing it needed to be
anything other than dead code -- a way to actually call it.

**Spec basis, cited not invented.** RFC 6733 section 8.3, Command-Code 258. Field-level definition
taken from this project's own vendored `simulators/reference/freeDiameter/libfdcore/
dict_base_proto.c` -- the same file ADR-0059 already established as this codebase's reference
oracle for Diameter base-protocol facts with no RFC text vendored alongside it. RAR's mandatory AVP
set (Session-Id, Origin-Host, Origin-Realm, Destination-Realm, Auth-Application-Id,
Re-Auth-Request-Type, Destination-Host) is at lines 2848-2899; RAA's (Session-Id, Result-Code,
Origin-Host, Origin-Realm) is at lines 2900-2955; the Re-Auth-Request-Type AVP (code 285, two real
enumerated values -- AUTHORIZE_ONLY=0, AUTHORIZE_AUTHENTICATE=1) is at lines 1846-1884. The new
dictionary constants in `libs/diameter-core/include/diameter_core/dictionary.hpp` (`Command::
kReAuth = 258`, `Avp::kReAuthRequestType = 285`, `ReAuthRequestType::kAuthorizeOnly/
kAuthorizeAuthenticate`) cite these exact line numbers, not approximated ones.

**What's built.** `DiameterServer::send_reauth_request(session_id, timeout = 5s)`
(`nfs/chf/src/diameter_server.hpp`/`.cpp`) -- CHF's first ever outgoing Diameter *request* (every
other message CHF builds is an Answer echoing a request's own Hop-by-Hop/End-to-End identifiers;
RAR is the first time CHF must generate its own, via a monotonic counter seeded from wall-clock
time at construction -- a disclosed, lab-adequate simplification of RFC 6733 section 3's fuller
"guaranteed unique at start-up" scheme). It looks up the session's live `PeerConnection` (a new
per-connection registry, `session_to_connection_`, populated at CCR-Initial and erased via the
existing `SessionCleanupGuard` RAII on every connection exit path), writes a real RAR built by
`build_rar()`, and blocks on a per-connection `condition_variable` for the matching RAA -- which
`handle_connection`'s own message loop now recognizes via a new branch (Command-Code 258, R-bit
clear) placed *before* the existing "unsupported command" rejection, since an RAA would otherwise
match neither that check (RAR/RAA was never a command CHF *received* unsolicited before now) nor
the request-flag check, and the connection would be closed on its own answer to CHF's own question.
`cap_server.hpp`/`.cpp` took the same `CatalogSnapshot&` threading `main.cpp` already needed (no
RAR-specific change there -- CAP/SS7 has no Diameter session to reauthorize).

**Real, disclosed simplification carried over from the header comment written while implementing
this:** only one RAR may be outstanding per *connection* (not per session) at a time -- a second
call targeting a different session multiplexed on the same connection while one is in flight waits
behind it rather than being independently correlated via its own Hop-by-Hop-Id lookup table. Real
Diameter deployments support concurrent in-flight requests per connection; building the full
per-request correlation table that would take is deferred, not silently assumed away -- this
project's first RAR implementation doesn't yet have concurrent-RAR-per-connection callers to need
it for. `send_reauth_request`'s three real "couldn't reach this peer" outcomes (unknown session,
peer write failed, no RAA within timeout) all collapse to `std::nullopt` -- RFC 6733's base protocol
doesn't distinguish them either at this layer, so this implementation doesn't invent a distinction
it would have to justify.

**The trigger: `POST /chf-admin/v1/sessions/{SessionId}/reauthorize`.** Without this,
`send_reauth_request()` would be real, tested-in-isolation-only dead code -- no reconciliation sweep
exists yet (ADR-0447 left that as a detection-only design with an open staleness threshold), and no
other internal caller exists either. Placed under the existing `/chf-admin/v1/...` prefix and
`check_bearer`-gated, the same as the real `/chf-admin/v1/policy-counters/{id}` route
(`nfs/chf/src/main.cpp`) -- deliberately **not** modeled on the unauthenticated `/internal-billing/
v1/runs` route: forcing a live Gy session's real re-authorization is a disruptive action on a
session a real PCEF/SGSN is actively using, closer in risk to an operator action (which
`/chf-admin/v1/...` already gates) than to a read/aggregate reporting trigger (which
`/internal-billing/v1/...` is). Not a real Nchf operation and not styled as one -- TS 32.291 has no
reauthorization-trigger operation, and RAR/RAA themselves are RFC 6733 base-protocol Diameter, not
an Nchf HTTP operation at all; inventing an Nchf path for triggering one would fabricate a 3GPP API
on top of fabricating the operation it triggers. Returns `{"sessionId", "resultCode"}` on 200, or
504 with an explanation covering all three of `send_reauth_request`'s own undistinguished failure
causes when it returns `std::nullopt` -- the HTTP layer doesn't claim more precision than the
Diameter layer underneath it actually has.

**Verification -- real wire protocol, not self-consistency.** New test file
`tests/integration/test_chf_diameter_rar.cpp`, two cases:

- `RealRarRaaRoundTripViaAdminEndpoint`: a fake Diameter peer (modeled on
  `test_chf_protocol_ceilings.cpp`'s raw-socket pattern) does a real CER/CEA handshake, then a real
  CCR-Initial (Session-Id + CC-Request-Type + CC-Request-Number -- the three mandatory AVPs
  `decode_ccr()` requires; no Subscription-Id/MSCC needed, since session registration into
  `session_to_connection_` happens unconditionally before CHF's per-MSCC `charge_one_usage` loop,
  so this test needs neither a live product-catalog/balance-management nor a real rated usage) and
  asserts a real CCA with Result-Code DIAMETER_SUCCESS. A background thread then waits on the same
  socket for the RAR CHF pushes once the foreground thread calls the new admin endpoint, asserts
  it's a real Request with Command-Code 258 carrying the *same* Session-Id (proving CHF targeted
  the real session, not a fabricated one), answers it with a real RAA (Result-Code 2001), and the
  foreground thread asserts the HTTP response's `resultCode` is the one that RAA actually carried.
  Passed, with CHF's own log independently confirming both ends: `chf: real RAR sent for session
  test-peer.example.com;rar-e2e;1 (Re-Auth-Request-Type=AUTHORIZE_ONLY)` followed by `chf: real RAA
  received (Result-Code=2001)`.
- `UnknownSessionReturns504`: calls the admin endpoint for a session no Diameter connection ever
  registered -- `send_reauth_request`'s own documented "no live connection holds this session"
  branch -- and asserts 504.

Both pass. The existing `ChfProtocolCeilings.*`/`CapScopedCharging.*`/`ChaosCharging.*`/
`BalanceLossless.*` suites (13 tests, covering every file this increment touched --
`diameter_server.cpp`/`.hpp`, `cap_server.cpp`/`.hpp`, `dictionary.hpp`, `main.cpp`) all still pass,
confirming no regression from threading the new registry/registration code through the existing
CCR-Initial/CCR-Termination paths. (A first local run of this suite showed 6 unrelated failures --
CHF FATALing with "direct-insert CDR mode is configured but Doris ... is unreachable"; real but
purely environmental, this machine's own `docker-doris-1` container was stopped at the time,
unrelated to anything in this ADR -- confirmed by re-running the identical suite after `docker
compose up -d doris`, which passed all 13.)

**How this relates to ADR-0447's still-open reconciliation sweep -- not a replacement, a better
tool for it.** The sweep's open question was a passive one: how long should a reservation sit
unclaimed before it's flagged "stuck," given that a real long-running session legitimately holds one
for a while and a fixed timeout can't tell the two apart. RAR changes what's available once that
sweep is built: instead of (or in addition to) a timeout guess, the sweep could send a real RAR at
the candidate session and use `send_reauth_request`'s own real answer as evidence -- a real RAA back
means the session is still alive and the reservation is legitimate, `std::nullopt` means the peer is
genuinely unreachable, which is a materially stronger signal than elapsed time alone. This is noted
as an available option for that future design, not decided here -- the sweep itself, and whether it
uses RAR as a probe or a timeout or both, remains unbuilt and unscheduled, same as ADR-0447 left it.

**Correction, found while considering building the sweep next: RAR does not unblock the staleness
question for most of CHF's own traffic.** RAR/RAA is Diameter-Gy specific -- it only reaches a
session that arrived over `DiameterServer`'s own TCP connection (`session_to_connection_` is
populated only at a real CCR-Initial). The majority of this project's charging traffic is the real
5G path, `Nchf_ConvergedCharging` over HTTP (N40/SMF, N28/PCF) -- TS 32.290/32.291 defines that
interface as client-initiated only; CHF has no spec-defined way to push anything at an SMF or PCF to
ask "is this session still live," RAR or otherwise. So for an HTTP-sourced stuck reservation (the
common case; Gy is this project's own legacy-voice-estate minority per `CLAUDE.md`), the probe-based
liveness check above provides no help at all, and ADR-0447's original open question -- what
staleness threshold distinguishes "stuck" from "a real long-running session" -- still has to be
asked about and answered before the sweep can be built, exactly as before this ADR. RAR narrows the
problem (a real option for the Gy subset) without solving it (no equivalent exists for the HTTP
majority). Recorded so this isn't misread later as "RAR closed the reconciliation-sweep gap" --
it didn't.

**Status: built, tested, not yet exercised by anything other than this test and a manual
operator call.** No reconciliation sweep consumes it. That is the real, current scope -- a real,
spec-conformant mechanism with a real way to invoke it, not yet wired into an automated process.

