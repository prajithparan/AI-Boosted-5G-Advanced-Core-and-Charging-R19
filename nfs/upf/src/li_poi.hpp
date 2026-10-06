#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

#include "li_core/x2x3_pdu.hpp"
#include "li_poi/poi_runtime.hpp"

// The UPF's CC-POI (TS 33.127 clause 6.2.3, TS 33.128 clauses 6.2.3.3 and 6.2.3.6, ADR-0464). The
// generic POI machinery is libs/li-poi; this keeps what is UPF-specific:
//
//  * LI_T3: the CC-TF in the SMF activates a task whose target is the PFCP session -- an F-SEID
//    TargetIdentifierExtension carrying the SEID the SMF chose (TS 33.128 table 6.2.3.3.1-2). The
//    SMF can send it BEFORE the N4 Session Establishment Request, which is what clause 6.2.3.3.1
//    wants ("when the SMF sends the N4: PFCP Session Establishment Request"): no packet of the
//    session precedes the task. Only the F-SEID kind is supported; the other UPFLIT3 identifiers
//    (F-TEID, UE IP, ports, PDR / QER ID, ...) are refused with X1 3010.
//  * the packet-tap seam: on_packet() is what the datapath calls for a packet of a PFCP session's
//    GTP-U tunnel. Nothing in this repository calls it from a real datapath yet -- the UPF's
//    eBPF/XDP program is uplink decapsulation only and cannot attach in CI -- so until the capture
//    stage exists a deployed UPF intercepts nothing. Tests feed it.
//  * LI_X3: each matching packet goes out as one TS 103 221-2 X3 PDU, Payload Format 12 (GTP-U,
//    the format clause 6.2.3.6 makes mandatory), direction FromTarget for uplink, ToTarget for
//    downlink.
namespace upf {

enum class PacketDirection : std::uint8_t { Uplink, Downlink };

class UpfLiPoi {
public:
    explicit UpfLiPoi(li_poi::Config config);
    ~UpfLiPoi();
    UpfLiPoi(const UpfLiPoi&) = delete;
    UpfLiPoi& operator=(const UpfLiPoi&) = delete;

    void start();
    void stop();

    // N4 bookkeeping: the session (CP F-SEID's SEID) and the UPF-allocated N3 uplink TEID.
    // `cp_address` is the SMF's address (the F-SEID is a SEID plus an address, and two SMFs may
    // choose the same SEID): a task whose F-SEID carries an address only matches that SMF's
    // session.
    void on_session_established(std::uint64_t cp_seid,
                                const std::string& cp_address,
                                std::uint64_t up_seid,
                                std::uint32_t uplink_teid);
    // A Session Deletion Request addresses the session by the UPF's own SEID.
    void on_session_deleted(std::uint64_t up_seid);

    // One GTP-U packet (with its GTP-U header) seen on the tunnel `teid`. A no-op unless a
    // provisioned warrant targets the session that tunnel belongs to.
    void on_packet(std::uint32_t teid,
                   PacketDirection direction,
                   std::span<const std::uint8_t> gtpu_packet);

private:
    li_poi::PoiRuntime runtime_;
    mutable std::mutex mutex_;
    struct Session {
        std::uint64_t seid = 0;
        std::string address;
        std::uint64_t up_seid = 0;
    };
    std::unordered_map<std::uint32_t, Session> session_by_teid_;
};

} // namespace upf
