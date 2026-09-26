#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "li_core/xiri.hpp"

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
// Wired events: Registration (TS 33.128 clause 6.2.2.2.2, ADR-0378), and since ADR-0440
// IdentifierAssociation (6.2.2.2.7, at REGISTRATION ACCEPT) and LocationUpdate (6.2.2.2.4, at N2
// PathSwitchRequest and HandoverNotify), all gated per target by the X1
// IdentifierAssociationExtensions parameter (6.2.2.2.1). Deregistration, IdentifierDeassociation
// and StartOfInterceptionWithRegisteredUE have codecs but no conformant AMF trigger yet (ADR-0440
// lists why) and are not emitted.

namespace amf {

// TS 33.128 table 6.2.2.1.1-2's per-target setting, as the POI stores it: Absent when the task
// carried no IdentifierAssociationExtensions.
enum class IdentifierAssociationGating : std::uint8_t { Absent, IdentifierAssociation, All };

// The AMF record types this POI can emit (the gating decision is per record type).
enum class AmfXiriRecord : std::uint8_t {
    Registration,
    LocationUpdate,
    IdentifierAssociation,
    IdentifierDeassociation,
};

// TS 33.128 clause 6.2.2.2.1, as a pure function:
//   Absent                -> every AMF record type EXCEPT Identifier(De)Association
//                            (table 6.2.2.1.1-1: "If the field is absent, AMFIdentifierAssociation
//                            and AMFIdentifierDeassociation records shall not be generated.")
//   IdentifierAssociation -> ONLY Identifier(De)Association and LocationUpdate ("No other record
//                            types shall be generated for that target.")
//   All                   -> every AMF record type.
bool xiri_record_enabled(IdentifierAssociationGating gating, AmfXiriRecord record);

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

    // Every report_* below is a no-op for a non-target SUPI, emits one xIRI per matching task
    // (two warrants on one UE each get their own, under their own XID and gating), and skips a
    // task whose gating disables that record type (xiri_record_enabled). Best-effort delivery.

    // TS 33.128 clause 6.2.2.2.2: AMFRegistration for a target UE that has just registered.
    void report_registration(const std::string& supi, const GutiParts& guti);

    // TS 33.128 clause 6.2.2.2.7: AMFIdentifierAssociation when the AMF sends REGISTRATION ACCEPT
    // (or CONFIGURATION UPDATE COMMAND with a 5G-GUTI) to a target UE -- "regardless of whether
    // the ... procedure is subsequently successfully completed or not". Table 6.2.2.2.7-1: sUPI,
    // gUTI and location (Location.locationInfo.userLocation) are M. Payload Direction 5.
    void report_identifier_association(const std::string& supi,
                                       const GutiParts& guti,
                                       const li_core::xiri::UserLocation& location);

    // TS 33.128 clause 6.2.2.2.4: AMFLocationUpdate when the target's location changes through UE
    // mobility -- the clause names the N2 Path Switch Request (TS 23.502 4.9.1.2) and the N2
    // Handover Notify (4.9.1.3). Table 6.2.2.2.4-1: sUPI and location (form 1,
    // Location.locationInfo.userLocation, for an NGAP source) are M.
    void report_location_update(const std::string& supi,
                                const li_core::xiri::UserLocation& location);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace amf
