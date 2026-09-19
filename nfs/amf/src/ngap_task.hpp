#pragma once

#include <cstdint>
#include <mutex>
#include <ngap_core/sctp_socket.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "aka_crypto/kdf.hpp"
#include "amf_ue_id_index_store.hpp"
#include "gnb_association_registry.hpp"
#include "peer_endpoints.hpp"
#include "ue_context_store.hpp"
#include "ue_security_context_store.hpp"

// AMF's NGAP/N2 termination (TS 38.413) + minimal NAS-5GS (TS 24.501), per docs/DECISIONS.md's
// staged NGAP/NAS plan (Stage 1: NG Setup; Stage 2: InitialUEMessage -> RegistrationRequest ->
// real AUSF call -> AuthenticationRequest). Runs on its own dedicated thread doing blocking SCTP
// I/O, never on the HTTP/2 server's io_context -- the same "blocking transport gets its own
// thread" discipline ADR-0006 already established for run_nrf_lifecycle, extended to SCTP by
// ADR-0030's libs/ngap-core.

namespace amf {
class LiPoi; // src/li_poi.hpp -- the AMF's LI IRI-POI (ADR-0377); nullptr when LI is disabled
}

namespace amf::ngap {

// Thread-safe registry mapping a UE's SUPI to enough live state for the SBI HTTP/2 server thread
// (route handlers, e.g. Namf_Communication N1N2MessageTransfer) to deliver a secured downlink NAS
// message to that UE's live NGAP association -- a genuine cross-thread handoff, since each NGAP
// association runs on its own dedicated thread doing blocking SCTP I/O (ADR-0030) while the SBI
// server runs on Boost.Asio's io_context thread. ADR-0038.
//
// Holds a non-owning pointer to the association's SctpSocket, valid only while that association's
// handle_association() call is still running -- register_ue/unregister_ue bracket that lifetime.
// All access serialized through this class's own mutex; in this build's current
// single-UE-per-association scope (ADR-0031) the owning NGAP thread is always blocked in
// SctpSocket::receive() (not sending) by the time an N1N2MessageTransfer can arrive for that UE,
// so there is no real send/send race today -- the mutex exists for correctness regardless of
// timing, not to paper over an observed race.
class NgapUeRegistry {
public:
    struct Entry {
        ngap_core::SctpSocket* socket = nullptr;
        std::uint32_t amf_ue_id = 0;
        std::uint32_t ran_ue_id = 0;
        aka_crypto::NasIntKey knas_int{};
        aka_crypto::NasEncKey knas_enc{};
        std::uint32_t next_downlink_count = 0;
    };

    void register_ue(const std::string& supi, Entry entry);
    void unregister_ue(const std::string& supi);

    // Encodes (amf::nas::encode_dl_nas_transport) and sends a secured DlNasTransport carrying
    // n1_sm_container over the registered UE's live association, incrementing its downlink_count.
    // Returns false if no live association is registered for this SUPI.
    bool send_dl_nas_transport(const std::string& supi,
                               std::uint8_t pdu_session_id,
                               const std::vector<std::uint8_t>& n1_sm_container);

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0096): sends a pre-encoded raw
    // NGAP PDU on the registered UE's CURRENT live association -- used by handle_handover_notify
    // (running on the TARGET association's own thread) to send a real, AMF-initiated
    // UEContextReleaseCommand back onto the SOURCE association BEFORE re-pointing this SUPI's own
    // entry to the target. Same cross-thread send safety precedent send_dl_nas_transport above
    // already established (the source thread is blocked in SctpSocket::receive(), not sending, by
    // the time this is called). Returns false if no live association is registered for this SUPI.
    bool send_raw(const std::string& supi, const std::vector<std::uint8_t>& pdu_bytes);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

// Blocks forever: binds SCTP on address:port, accepts gNB associations, and handles NGAP PDUs for
// each one -- making real SBI calls to AUSF (Stage 2/3) and, once registration completes, to PCF
// (Stage 5, storing the resulting PolicyAssociation in `ue_contexts`). amf_instance_id and
// nrf_base are needed to build those clients' own OAuth2Client. Call from a dedicated std::thread.
// `ue_contexts`/`ue_ngap_registry`/`ue_security_contexts` must outlive this call (it runs
// forever) -- same shared-by-reference-across-threads convention as main()'s other stores
// already use with the HTTP/2 server's route handlers.
//
// ue_security_contexts/amf_region_id/amf_set_id/amf_pointer: gap-closure (
// docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075) -- the real, persistent NAS security
// context store ServiceRequest needs, plus this AMF instance's own real, disclosed lab 5G-GUTI
// identity (see main.cpp's own kAmfRegionId/kAmfSetId/kAmfPointer comment), threaded through so
// RegistrationAccept can assign a real GUTI and a later ServiceRequest can be routed back here.
//
// amf_ue_id_index: gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0090) -- the real
// amf_ue_ngap_id -> tmsi cross-association index PathSwitchRequest needs, see
// amf_ue_id_index_store.hpp's own comment. Real, disclosed correction to this function's own
// pre-existing header comment above ("each NGAP association runs on its own dedicated thread"):
// found, while building this, to not match the real implementation -- this project's actual NGAP
// accept loop handles one association at a time, sequentially, on ITS OWN single thread (`while
// (true) { assoc = listener.accept(); handle_association(...); }`, no per-association
// std::thread spawn anywhere in this file); NgapUeRegistry's own cross-THREAD documentation
// still describes the real cross-thread handoff with the SBI HTTP/2 server thread correctly, just
// not a per-association thread on the NGAP side. Not fixed here (out of this ADR's own scope,
// and PathSwitchRequest's own design already accounts for the real sequential-not-concurrent
// behavior by reading persisted Redis state rather than live per-association memory).
// gnb_associations: gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0095/ADR-0096) --
// the real cross-association relay registry a genuine N2-based handover
// (HandoverRequired/.../HandoverNotify) needs. Real, disclosed change to this function's own
// accept loop alongside it: now one std::thread per accepted association (was strictly
// sequential, ADR-0031) so AMF can genuinely hold a source AND a target gNB association open at
// the same time -- see gnb_association_registry.hpp's own header comment.
void run_ngap_lifecycle(const std::string& bind_address,
                        unsigned short bind_port,
                        const std::string& amf_instance_id,
                        const std::string& nrf_base,
                        const PeerEndpoints& peers,
                        UeContextStore& ue_contexts,
                        NgapUeRegistry& ue_ngap_registry,
                        UeSecurityContextStore& ue_security_contexts,
                        AmfUeIdIndexStore& amf_ue_id_index,
                        GnbAssociationRegistry& gnb_associations,
                        std::uint8_t amf_region_id,
                        std::uint16_t amf_set_id,
                        std::uint8_t amf_pointer,
                        LiPoi* li_poi);

} // namespace amf::ngap
