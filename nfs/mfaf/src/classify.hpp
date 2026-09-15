#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

#include "TS26510_CommonData_grp.hpp" // the 3CA DTOs live in the SCC group header since TS 29.574 joined codegen (ADR-0366)

// What arrives at the MFAF Notification Target Address is the Data Source's OWN notify body --
// Namf_EventExposure_Notify's AmfEventNotification, Nsmf_EventExposure's
// NsmfEventExposureNotification, and so on (TS 23.288 6.2.6.3.4 step 7). TS 29.576 defines no
// schema for that inbound message; what it defines is the OUTBOUND NmfafDataAnaNotification,
// whose DataNotification (TS 29.575) has one typed bucket per source NF. So the MFAF has to
// decide which bucket a body belongs in.
//
// First by the 3gpp-Sbi-Callback header (TS 29.500 5.2.3.2.3: "the name of the notify service
// operation", e.g. Namf_EventExposure_Notify -- Annex B table B-1), matched on its API-name
// prefix so either the operation name or the YAML callback name resolves. Failing that, by body
// shape, only where a shape is unambiguous: AMF, NRF, UDM, NWDAF, NSACF, UPF, GMLC, LMF carry a
// distinguishing member; SMF, NEF, AF and PCF all serialise as {notifId, eventNotifs} and cannot
// be told apart without the header. An inbound that resolves to nothing is counted and dropped --
// forwarding it in the wrong bucket would be a lie to the consumer.

namespace mfaf {

struct Classified {
    sbi_gen::NmfafDataAnaNotification notification;
    std::string source; // "AMF", "SMF", ... "NWDAF" -- for the log line and the counter label
};

std::optional<Classified> classify(std::optional<std::string_view> callback_header,
                                   const nlohmann::json& body);

} // namespace mfaf
