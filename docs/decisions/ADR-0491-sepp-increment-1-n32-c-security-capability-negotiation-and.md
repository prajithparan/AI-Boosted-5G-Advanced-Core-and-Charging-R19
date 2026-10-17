## ADR-0491: SEPP increment 1 -- N32-c Security Capability Negotiation and N32-f forwarding with TLS security (TS 29.573)

**Date:** 2026-10-14. **Status:** Accepted -- increment 1 built (procedure list approved by the user, 2026-10-14).

**Context.** The SEPP is the Tier-2 NF whose absence blocks any inter-PLMN SBI traffic (no N32).
Specs in hand: TS 29.573 j70 (fetched), TS29573_N32_Handshake.yaml (REL-19, info.version 1.4.0;
added to the codegen pilot list -- only N32Purpose had been generated, via the NRF YAML), TS 29.500
j70 6.1.4.3, TS 23.003 j80 28.2 / 28.5 (fetched).

**Approved procedure list (increment 1).**
- 5.2.2 Security Capability Negotiation (PostExchangeCapability): TLS selected; "NONE" tears the
  N32-f TLS connection down; collision rule (the lexicographically preceding FQDN wins, 409
  N32C_EXCHANGE_CAPABILITY_ONGOING); purposes (default ROAMING + INTER_PLMN_MOBILITY when none are
  exchanged; 403 REQUESTED_PURPOSE_NOT_ALLOWED when all are rejected); n32HandshakeId echoed.
- 5.2.4 N32-f Context Termination and 5.2.5 N32-f Error Reporting: served (see disclosures).
- 5.3.3 forwarding with TLS security: TS 29.500 6.1.4.3.4 second case (both SEPPs indicate
  3gpp-Sbi-Target-apiRoot support): HTTPS to the peer SEPP's apiRoot, the header forwarded
  unmodified, 3gpp-Sbi-N32-Handshake-Id added (removed by the receiving SEPP), Via appended on
  request and response; the receiving SEPP correlates the request to its N32-c context per
  5.3.3.2.1 (certificate FQDN; Via when the certificate names several SEPPs; handshake id or
  purpose with several contexts), else 403 CONTEXT_NOT_FOUND (table 5.3.3.4-1).
- Later increments: 5.4 telescopic FQDN mapping; 5.2.3 parameter exchange + 5.3.2 PRINS / JOSE;
  5.5 roaming intermediaries.

**Decisions.**
1. `nfs/sepp`: two TLS 1.3 + mTLS servers -- the SBI port (NFs of its own PLMN) and the N32 port
   (partner SEPPs: N32-c and N32-f). Partners, purposes, own FQDN/PLMN in `config/sepp.json`.
   Lab: PLMN 001/01 (`sepp.5gc.mnc001.mcc001.3gppnetwork.org`, 7820/7821), partner 001/02 (7822/7823
   in the test).
2. A request's target PLMN is the Home Network Domain its target host ends with (TS 23.003 28.5.1),
   compared in domain form (MNC padded to three digits), never by re-parsing digits.
3. Lab PKI: the `sepp` certificate additionally carries both lab SEPP FQDNs (LAB_SEPP_FQDNS in
   scripts/gen-lab-pki.sh) -- one common certificate for both partners, the case 5.3.3.2.1's NOTE
   resolves with the Via header, which is what the code does. `sepp` added to CI's PKI lists.
4. `libs/sbi-core` Server: method `*` and a trailing `**` pattern segment, so a relay can serve
   every API path; routes are tried in registration order (N32-c routes first).
5. Errors use TS 29.500 table 5.2.7.2-1 / TS 29.573 table 6.1.6.3-1 causes and carry
   `Server: SEPP-<FQDN>` (TS 29.500 5.2.2.2). No N32 context with the target PLMN, or the next hop
   unreachable: 504 TARGET_NF_NOT_REACHABLE.

**Disclosed.**
- TLS security only: an offer of PRINS alone gets 403 NEGOTIATION_NOT_ALLOWED, and so does
  exchange-params. Consequently no n32fContextId exists, so n32f-terminate always answers 404 and
  n32f-error (which reports PRINS-reformatted messages) is only logged + 204.
- The receiving SEPP reaches NFs of its own PLMN through `local_nf_routes` (target authority ->
  reachable apiRoot), standing in for DNS of the 3gppnetwork.org FQDNs, which the lab does not run.
- A peer that does not support 3gpp-Sbi-Target-apiRoot (6.1.4.3.4 first case: the target URI on
  N32-f) is not served (504 with that reason); telescopic FQDNs are not built (400 without the
  header).
- Not built: N32-f keepalive PINGs (5.3.3.5), TLSCOR OPTIONS (5.3.3.2.2), SNDN32F, roaming
  intermediaries, topology hiding, message filtering / PLMN-ID checks of tokens, OAuth2 of the
  relayed NF traffic (tokens are for the producer). Registered with the NRF as nfType SEPP (fqdn,
  plmnList, heartbeat) but without SeppInfo; NFs do not discover the SEPP through the NRF here.
- The compose lab has no partner SEPP: the compose SEPP keeps retrying exchange-capability towards
  config/sepp.json's partner (logged) until one is configured.
- Unknown sender FQDN -> 403 NEGOTIATION_NOT_ALLOWED ("no security capability configured for this
  SEPP"); a targetPlmnId this SEPP does not serve -> 400 OPTIONAL_IE_INCORRECT: the spec names no
  cause for either, these are the closest defined ones.

**Rejected alternatives.** Forwarding with the target URI itself on N32-f (6.1.4.3.4 first case):
needs connect-to routing in the SBI client and certificates for remote NF FQDNs; the second case
is the one both SEPPs here support. Correlating N32-f by handshake id alone: 5.3.3.2.1 says the
certificate FQDN shall be used when one context exists. A per-PLMN certificate pair for the lab:
two certificates for one lab host, where the spec explicitly covers the common-certificate case.

**Verified.** `SeppN32.*` (7: domains incl. TS 23.003's own MCC 345 / MNC 12 example; 5.2.2
selection, refusals, collision, NONE; 5.3.3.2 correlation) and `structural_conformance` (the
SecNegotiateReqData / SecNegotiateRspData this SEPP builds, against TS29573_N32_Handshake.yaml);
`SeppEndToEnd` (2): two real SEPPs negotiate, a consumer in PLMN A gets PLMN B's real NRF
Bootstrapping response through both (Via names both SEPPs); an unknown PLMN gets 504 from SEPP A;
an N32-f request without a context gets 403 CONTEXT_NOT_FOUND; an unconfigured SEPP's
exchange-capability gets 403 NEGOTIATION_NOT_ALLOWED.

**Definition-of-done items completed (2026-10-14, second commit).** NRF registration + heartbeat
(seen: "registered with NRF (HTTP 201)" in `SeppEndToEnd`); Prometheus counters
`sepp_n32c_handshakes_total`, `sepp_n32f_forwarded_total`, `sepp_n32f_refused_total`;
`deploy/docker/sepp.Dockerfile`, a compose `sepp` service with the `config:ro` mount (ADR-0453) and
`sepp` in `pki-init`; `deploy/helm/sepp`; the architecture tile solid (`docs/diagrams/`), README.

**Review fix (2026-10-15, independent review).** N32-c did not bind the `sender` IE to the
authenticated peer: exchange-capability (and exchange-params) stored the context against whatever
FQDN the body claimed. Now the claimed sender must be one of the client certificate's dNSName SANs,
else 403 NEGOTIATION_NOT_ALLOWED (TS 29.573 6.1.5.3.2: the IE "shall uniquely identify the SEPP that
is sending the request"). Residual, disclosed: the lab's one shared `sepp` certificate names both lab
SEPPs, so in the lab either can still claim the other; with per-SEPP certificates the binding is
exact. Also: the telescopic Location rewrite now matches case-insensitively and skips a non-match
(it could throw on a mixed-case host). Verified: `SeppEndToEnd.AnN32fRequestFromAPeerWithoutAContextIsRefused`
now also sends exchange-capability claiming SEPP A with the hello-nf certificate -> 403.

**Review fix SEC-02 (2026-10-16, docs/reviews/REVIEW-5ae0ef1.md).** N32-f correlation over TLS
applied 5.3.3.2.1's several-contexts rule across *different* peers and skipped the certificate
check there: with two or more partners, a request was mapped to a context by the
`3gpp-Sbi-N32-Handshake-Id` header alone, whoever presented the certificate, and two partners that
both negotiated ROAMING without handshake ids were "ambiguous", so every N32-f request got 403.
The clause's rule is for several N32-c contexts "between a pair of SEPPs". `sepp::correlate` now
identifies the peer first -- contexts whose peer FQDN is a client-certificate dNSName SAN, narrowed
by the Via header's `SEPP-<fqdn>` entries when the certificate names several SEPPs -- refuses when
more than one peer remains, and only then applies the handshake-id / purpose rule among that peer's
contexts. `SeppN32.CorrelationFollowsClause5332` encoded the misreading (two peers correlated by
handshake id with an empty certificate-name list); it now asserts the opposite and fails on the old
code (checked). New: `SeppN32.CorrelationAmongOnePeersContextsUsesHandshakeIdThenPurpose`.
Residual, unchanged: the store keeps one context per peer, so the one-peer several-contexts branch
is not reachable from the running SEPP yet; per-PLMN trust anchors (SEC-01) are not built.
