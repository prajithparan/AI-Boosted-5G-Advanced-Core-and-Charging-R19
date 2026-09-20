#pragma once

#include <cstdint>
#include <memory>
#include <string>

// The AMF's Lawful Interception IRI-POI (TS 33.127 clause 6.2.2, ADR-0377 programme). Private to
// nfs/amf.
//
// TS 33.127 keeps LI off the service-based plane, so this is NOT part of the AMF's Namf SBI
// surface: it runs its own LI_X1 provisioning listener (ADMF -> AMF, its own port and mTLS, the
// same li_core::x1 NE server the MDF2 uses) and streams xIRIs to the MDF2 over LI_X2 with a
// li_core::X2X3Client. It shares no state with the AMF's UE handling beyond the SUPI/GUTI the
// hot-path hands it at an event.
//
// Disabled unless config/amf.json's li_poi.enabled is true; when disabled the AMF never
// constructs one, callers hold a nullptr, and every hook is a no-op -- the AMF behaves exactly as
// it did before the POI existed. Delivery is best-effort: a dead or slow MDF2 is logged and
// counted, never allowed to break a UE procedure (an intercept failure must not take down the
// network function it lives in).
//
// First slice (this file): the LI_X1 server + target store + LI_X2 emit, wired to the
// Registration event only (TS 33.128 clause 6.2.2.2.2). The other five approved events'
// hooks -- Deregistration, LocationUpdate, StartOfInterception, Identifier(De)Association, whose
// li_core::xiri codecs already exist -- are follow-up slices.

namespace amf {

// The 5G-GUTI components the AMF has in hand at registration Stage 5 (the allocated 5G-TMSI plus
// this AMF instance's own region/set/pointer and serving PLMN). The POI maps these onto the
// AmfRegistration xIRI's FiveGGUTI.
struct GutiParts {
    std::string mcc;
    std::string mnc;
    std::uint8_t amf_region_id = 0;
    std::uint16_t amf_set_id = 0;
    std::uint8_t amf_pointer = 0;
    std::uint32_t five_g_tmsi = 0;
};

class LiPoi {
public:
    struct Config {
        std::string x1_bind_address;       // LI_X1 listener bind address
        std::uint16_t x1_port = 0;         // LI_X1 listener port (distinct from the Namf port)
        std::string ne_identifier;         // this NE's identifier, echoed on X1 responses
        std::string network_function_id;   // X2 NFID attribute (TS 33.128 5.3.1) -- the AMF's id
        std::string interception_point_id; // X2 IPID attribute
        std::string mdf2_host;             // LI_X2 destination (the MDF2)
        std::uint16_t mdf2_port = 0;
        std::string mdf2_sni;  // server name to require in the MDF2's cert
        std::string cert_path; // PEM: mTLS client/server material (shared lab PKI)
        std::string key_path;
        std::string ca_path;
        int x1_keepalive_p1_seconds = 60; // TS 103 221-1 6.6.2 timers
        int x1_keepalive_p2_seconds = 180;
        int x1_keepalive_p3_seconds = 300;
        bool x1_allow_deactivate_all = true;
    };

    explicit LiPoi(Config config);
    ~LiPoi();
    LiPoi(const LiPoi&) = delete;
    LiPoi& operator=(const LiPoi&) = delete;

    // Start the LI_X1 provisioning listener (its own io_context on its own thread) and the
    // clause-6.6.2 keepalive monitor. The LI_X2 client connects lazily on the first emit.
    void start();
    void stop();

    // True if an active warrant targets this SUPI. Accepts the AMF's "imsi-<digits>" SBI form or
    // bare digits; matches against SUPI/IMSI target identifiers provisioned over LI_X1.
    [[nodiscard]] bool is_target(const std::string& supi) const;

    // TS 33.128 clause 6.2.2.2.2: emit an AMFRegistration xIRI for a target UE that has just
    // registered. No-op if the SUPI is not a target. Best-effort delivery.
    void report_registration(const std::string& supi, const GutiParts& guti);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace amf
