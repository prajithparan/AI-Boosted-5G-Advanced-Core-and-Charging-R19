## ADR-0095: real concurrent NGAP association handling (prerequisite for N2 handover)

### Context

Scoping task #100's remaining N2-based handover chain (`HandoverRequired`/.../`HandoverNotify`)
found a real, blocking architectural constraint before any handover-specific code could be
written: `run_ngap_lifecycle`'s own accept loop (`nfs/amf/src/ngap_task.cpp`) handled gNB
associations strictly sequentially -- `while (true) { assoc = listener.accept();
handle_association(std::move(assoc), ...); }`, where `handle_association` blocks for the
association's entire lifetime. This was already a real, disclosed lab-scope decision (ADR-0031:
"a real AMF would handle multiple concurrent associations"), not a bug -- but it means AMF could
never hold two gNB associations open at the same time, for ANY reason. Real N2 handover
structurally needs exactly that: AMF must relay `HandoverRequest` onto the TARGET gNB's own live
association while the SOURCE gNB's association is still open, waiting for `HandoverCommand`.

Surfaced to the user before writing handover-specific code (AskUserQuestion): build the real fix
(concurrent associations) as a prerequisite this same pass, or accept a narrower single-association
verification tier with the concurrency gap left open. User chose the real fix.

### Decision and implementation

- `run_ngap_lifecycle`'s accept loop now spawns one `std::thread` per accepted association
  (detached -- this lab has no coordinated shutdown path for in-flight associations, same
  disclosed scope every other detached thread in this project already carries), instead of
  handling one at a time.
- Real, load-bearing correctness fix found while making this change: `ausf_client`/`pcf_client`/
  `smf_client` (`sbi_core::http2::Client`, documented as synchronous/not thread-shared,
  ADR-0006/ADR-0027) were previously constructed ONCE in `run_ngap_lifecycle` and shared by
  reference across every sequentially-handled association -- safe when only one association ran
  at a time, a real race the moment two run concurrently. Fixed by moving their construction into
  a new `run_association_thread` (one dedicated client set per spawned thread, matching the
  established "separate thread gets its own separate client" discipline exactly, just now applied
  per-association instead of per-NF).
- New `nfs/amf/src/gnb_association_registry.{hpp,cpp}` (`amf::ngap::GnbAssociationRegistry`): a
  real, thread-safe registry mapping a gNB's own real identity (the PER-encoded bytes of its
  `GlobalGNB-ID` IE, TS 38.413 §9.3.1.6 -- not an invented label) to a live association handle,
  supporting `send_and_await_reply` (source thread sends onto the target's association, blocks up
  to 10s on a condition variable for the target's own receiving thread to deliver a correlated
  reply) and a plain fire-and-forget `send`. Real, disclosed lab-scope simplification: one relay
  in flight per target gNB at a time (matches ADR-0031's own "one gNB/one UE at a time" precedent,
  extended).
- Real `GlobalRANNodeID` extraction at `NGSetupRequest` (`extract_global_gnb_id`, id-GlobalRANNodeID=27)
  -- only the real `globalGNB-ID` CHOICE arm is supported (this project's only real RAN node type);
  `globalNgENB-ID`/`globalN3IWF-ID`/the TNGF/TWIF/W-AGF extension IEs are rejected, not silently
  misparsed as a gNB. Registered into `GnbAssociationRegistry` on success, unregistered on
  association close.
- `NgapUeRegistry` gained `send_raw` (a pre-encoded-PDU send on a SUPI's current live
  association) -- used by ADR-0096's `handle_handover_notify` to send a real, AMF-initiated
  `UEContextReleaseCommand` cross-thread onto the (still-current) source association, same
  cross-thread send-safety precedent `send_dl_nas_transport` already established.

### Testing and verification

`amf` built clean, zero new warnings. See ADR-0096 below for the full live, cross-process,
two-concurrent-association verification -- this ADR's own concurrency fix is what made that
possible at all (confirmed live: `amf-ngap: gNB association established` logged twice, for two
genuinely simultaneously-open associations, not sequential).

### What this ADR does NOT include

A coordinated graceful shutdown for in-flight association threads (detached, same disclosed class
as every other fire-and-forget thread in this project). A generic per-transaction correlation
scheme for multiple concurrent relays to the same target gNB (real, disclosed lab-scope
simplification -- one at a time per gNB). Real load/stress testing of many concurrent
associations (a real, separate, future carrier-grade-hardening concern, ADR-0049).

