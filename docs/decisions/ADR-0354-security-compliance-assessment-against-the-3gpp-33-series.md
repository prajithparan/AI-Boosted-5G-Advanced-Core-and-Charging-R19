## ADR-0354: security compliance assessment against the 3GPP 33-series (Release 19)

**Date:** 2026-09-13. **Status:** accepted. User-directed: fetch the listed 33-series
specifications, read them, record what is missing from a security perspective, and put the
compliance picture in the README.

The assessment itself is `docs/SECURITY_COMPLIANCE.md`. This entry records the decisions made in
producing it and the method, so the document's claims can be re-derived.

### Sources, and one that does not exist

`tools/specs/fetch_3gpp_specs.py` lists each spec's directory in the official 3GPP archive,
takes the newest Release-19 file (3GPP encodes the release as the version's first letter, `j` =
Rel-19), converts it with LibreOffice, and writes `specs/3gpp/MANIFEST.tsv` recording the exact
version fetched and -- had any spec lacked a Rel-19 file -- which release was used instead. All 21
real specs had a Rel-19 file, so no substitution was needed; the column exists so a future run
cannot silently report "Release 19" for a document that has none.

TS 33.258 was requested and does not exist: the archive's 33-series holds 33.250, 33.256 and
33.259. Recorded as such. Inventing a TS number is the one failure this project treats as worst,
and reporting compliance against a document that is not there would be exactly that.

### Method, stated at the strength it deserves

The corpus is 109,817 lines. "Read completely and in depth" was interpreted as: read every
specification's Scope to settle applicability; map every clause; read in full every clause that
places a requirement on an NF this project builds or has in scope; check each such requirement
against the code, a test, or a live experiment; cite both. Clauses whose only subject is a
component this project does not build were read for scope and marked out of scope. Rows in the
document are labelled implemented / not implemented / not assessed, and "not assessed" means
exactly that -- it is not a soft "probably fine".

### What was found, in the order it matters

1. **Lawful Interception is absent in its entirety**, and TS 33.127 requires it of every
   control-plane NF this project has, including an IRI-POI in the CHF (clause 7.22). This is the
   finding that changes the commercialization picture: a core that cannot be intercepted cannot
   be deployed by a licensed operator. It is not on any roadmap in this repository and needs to be.
2. **128-NEA1/NIA1 (SNOW 3G) are mandatory for the AMF and not implemented.** Already disclosed
   in `nas_security.hpp`, but disclosed as "out of scope", which TS 33.501 §5.5 does not permit.
3. **Duplicate JSON keys are silently accepted**, against TS 33.117 §4.3.6.3, verified by running
   the project's own parser on a duplicate-key payload. A genuine parser-differential attack
   surface, fixable in one place.
4. **NRF does not filter discovery by the producer's authorization parameters** (33.501 §13.3.1.3).
5. **TLS 1.3 only**, where TS 33.210 §6.2.1 requires TLS 1.2 support as well. Left as a decision
   for the architect: the non-conformant choice is the more secure one.
6. **SNI is never sent** because every lab URL is an IP literal. A configuration gap CI inherits.
7. **SBA certificate profile not verifiable** without TS 33.310, which was not requested.

Two things were also found to be *better* than recorded: the OAuth2 tokens are ES256-signed
(CLAUDE.md still called them "unsigned fake"), and every SBI runs TLS 1.3 + mTLS (CLAUDE.md still
called the transport "h2c-only"). Both debt notes corrected.

### Decisions

- **TS 33.528 is not assessed against.** Its own line 14 says it has not been approved and shall
  not be implemented; its body defers to TS 33.117. Assessing against it would be assessing
  against a placeholder.
- **TS 33.106/107/108 are recorded as superseded**, on the authority of 33.106's own Scope, not on
  this project's judgement.
- **The GBA BSF and this project's BSF are not the same thing** and the document says so, because
  the name collision is precisely the kind that gets a spec ticked off by mistake.
- **No fix was applied in this pass.** The user asked for the gaps to be recorded. Each fix -- LI
  above all -- is its own piece of work with its own ADR.

### Rejected

- Treating "read completely" as reading 2,500 pages linearly and then summarising from memory. The
  clause-mapped, requirement-by-requirement method produces citations; a linear read produces
  impressions.
- Marking the out-of-scope specifications "compliant". They are not compliant; they do not apply.
  The table keeps the two apart.

