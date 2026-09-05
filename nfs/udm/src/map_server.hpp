#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "ss7_core/sctp_socket.hpp"
#include "stores.hpp"

// ADR-0299: UDM as a real MAP SERVER (the HLR role on Gr/Gc), private to nfs/udm.
//
// Why this exists rather than "more codecs". MAP operations have a direction, and it decides which
// side of this project has to change:
//
//   insertSubscriberData, cancelLocation, deleteSubscriberData   HLR -> VLR   UDM SENDS
//   updateLocation, sendAuthenticationInfo, purgeMS              VLR -> HLR   UDM RECEIVES
//   checkIMEI, sendIdentification                                not the HLR's operations at all
//
// ADR-0293 and ADR-0296 built the sending side. The three receiving operations cannot be reached
// by a client no matter how many argument encoders exist -- adding those encoders alone would have
// produced exactly the unreachable code ADR-0293 was written about. They need a server, and this
// is it.
//
// Shape follows nfs/chf/src/cap_server.hpp deliberately: an SCTP listener, a dedicated accept
// thread, one thread per association, the real M3UA ASPSM/ASPTM handshake in the responder role.
// That server is the working precedent for a TCAP dialogue served over this stack, so this reuses
// its structure rather than inventing a second one.
//
// Real, disclosed scope for this increment:
//   * updateLocation -- answered with this UDM's own configured hlr-Number, and the VLR recorded
//     against the subscriber so a later cancelLocation has somewhere to go.
//   * purgeMS -- the subscriber's serving-node record is dropped; the real freezeTMSI/freezeP-TMSI
//     flags are NOT set (they instruct the VLR to quarantine a TMSI, and nothing in this project
//     allocates or tracks TMSIs, so setting them would be a claim about state that does not exist).
//   * sendAuthenticationInfo -- NOT implemented. It is a genuine capability gap, not an oversight:
//     the response carries an AuthenticationSetList of quintuplets, an ASN.1 structure this codec
//     does not model. UDM has the real vectors (Nudm_UEAU/milenage) -- only the MAP encoding is
//     missing. An unimplemented opcode is answered with a real ReturnError, never ignored.
//
// OFF unless `map_server_port` is configured, like every other listener this project has added.

namespace udm {

class MapServer {
public:
    // Binds 0.0.0.0:port and starts the accept thread. `hlr_number` is this UDM's own real
    // ISDN-AddressString, echoed in every UpdateLocationRes -- it comes from config, never a
    // literal (task #109). `amf_registrations` is the SAME store the Nudm_UECM HTTP handlers use:
    // a VLR registering a subscriber over MAP and an AMF registering one over SBI are the same
    // fact about the same subscriber, and keeping two stores would let them disagree.
    MapServer(std::uint16_t port,
              std::vector<std::uint8_t> hlr_number,
              AmfRegistrationStore& amf_registrations);
    ~MapServer();

    MapServer(const MapServer&) = delete;
    MapServer& operator=(const MapServer&) = delete;

private:
    void accept_loop();
    void handle_connection(ss7_core::SctpSocket socket);

    ss7_core::SctpSocket listener_;
    std::vector<std::uint8_t> hlr_number_;
    AmfRegistrationStore& amf_registrations_;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
};

} // namespace udm
