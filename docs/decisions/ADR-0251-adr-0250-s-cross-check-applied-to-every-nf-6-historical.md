## ADR-0251: ADR-0250's cross-check applied to every NF -- 6 historical staleness errors fixed

### Context

User instruction: apply ADR-0250's YAML/TS cross-check across **all** NFs and fix historical
issues, with the precedence stated explicitly -- **the YAML is the prime authority for API shape,
the TS is the reference.** That precedence is now recorded as a standing rule.

### A methodology error caught in the act, worth recording

The first sweep grepped the code for `supi-or-gpsi`, `sor-ack` and `ue-reachability` -- names
inferred from *operationIds* -- got zero hits, and was about to conclude those operations were
missing despite ADRs claiming them closed. Checking the YAML first showed the real paths are
`/{ueId}/id-translation-result`, `/{supi}/am-data/sor-ack` and `/multiple-identifiers`. All four
were in fact implemented; the greps were wrong, not the code.

**`operationId` is not a path.** Deriving one from the other is exactly the single-source shortcut
ADR-0250 exists to prevent, and it produced a false negative within minutes of starting. Every
subsequent check in this audit resolved the real path from the YAML first.

### Historical errors found and fixed

`docs/CAPABILITY_GAP_ANALYSIS.md`'s summary table -- the document the project uses to decide what
to build next -- was stale for **5 of its 9 originally-swept NFs**, every one in the
"highest-priority real gap" column, i.e. the column that directs work:

| NF | Was listed as the top open gap | Reality |
|---|---|---|
| NRF | NFProfile semantic validation; heartbeat-expiry timer | Both CLOSED -- `validate_nf_profile` is real and called on register; sweep landed ADR-0079 |
| AUSF | `Nausf_SoRProtection`, ProSe auth | Both CLOSED (ADR-0091) |
| PCF | `Npcf_PolicyAuthorization` | CLOSED by ADR-0080 -- and this one nearly caused a full reimplementation this session before the check caught it |
| UDM | `Nudm_EE`/`Nudm_PP` | Both CLOSED (ADR-0082, ADR-0237) |
| SMF | "the other 20 N2SmInfoType values" | Now 22 of 26 -- the count was never updated after ADR-0248/0249 added two |

Verified as genuinely still open, not merely assumed: **AMF's `HandoverCancel`** and the
**AMF->SMF handover relay** (ADR-0249's own disclosure stands).

**Sixth error, inside the source:** `nfs/udm/src/main.cpp`'s header comment listed `Nudm_EE`,
`Nudm_MT`, `Nudm_NIDDAU`, `Nudm_PP`, `Nudm_RSDS`, `Nudm_SSAU`, `Nudm_UEID` and SDM's
"~25 remaining GET operations" as "deliberately deferred, not dropped" -- while **the same file,
40 lines below, documents every one of them as implemented.** The comment contradicted the code
beneath it. Corrected, with the genuinely-still-deferred items (UECM's non-3GPP-AMF/SMSF/
IP-SM-GW/NWDAF registration groups, UEAU's GetRgAuthData/GenerateAv/GenerateGbaAv) kept and
labelled as verified rather than carried over on faith.

### Why this class of error matters more than it looks

None of these were code defects; every one was a **navigational** defect. The gap-analysis table
is what this project consults to choose the next task, so a stale row does not merely misinform --
it actively directs effort at work already done. That is not hypothetical: `Npcf_PolicyAuthorization`
was selected as this session's next capability target on the strength of that row, and only a
verify-before-acting check stopped it. The cost of the stale row was nearly a duplicated NF
service.

### What was checked and found clean

- All 37 `sbi_gen::<Enum>::<VALUE>` usages across `nfs/`, `bss/` and `libs/` resolve to real
  generated (therefore YAML-derived) values. Noted honestly: this check is compile-enforced and
  cannot fail, so it is weak evidence -- recorded for completeness, not as reassurance.
- NRF's TS 28.552 `NFS.*` mapping re-verified against the YAML operations
  (`RegisterNFInstance`/`UpdateNFInstance`/`SearchNFInstances`): holds.
- The 26-value `N2SmInfoType` enum: holds.

### Honest limits of this audit

A full path-by-path reconciliation of every routed endpoint against every YAML path across all 17
NFs was attempted and **abandoned as unreliable**: route registration concatenates API-root
constants across multiple files, and the extractor under-counted badly (it reported 12 routes for
UDR, which demonstrably has far more). Rather than publish numbers known to be wrong, this ADR
reports only what was verified individually. A trustworthy automated reconciliation needs the
route table extracted at build time from the binaries themselves, not regex over source -- a real,
named follow-up, not a silent omission.

