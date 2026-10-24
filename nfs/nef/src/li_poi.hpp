#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

#include "li_core/xiri.hpp"
#include "li_poi/poi_runtime.hpp"

// The NEF's Lawful Interception IRI-POI (TS 33.127 clause 7.9, TS 33.128 clause 7.7, ADR-0487).
// Private to nfs/nef. Shares the generic X1/X2 runtime (libs/li-poi) with the AMF, SMF, UDM and
// SMSF POIs; what stays here is NEF-specific.
//
// Wired: TS 33.128 clause 7.7.6.1.2 -- NEFAFSessionWithQoSProvision when the NEF returns its
// response to an AF's AsSessionWithQoS create (POST), update (PUT / PATCH) or revoke (DELETE) for a
// subscription naming a target GPSI.
//
// Target formats accepted over LI_X1 (clause 7.7.1.2): SUPIIMSI, SUPINAI, GPSIMSISDN, GPSINAI.
// Disclosed: the record is keyed on gPSI (mandatory) and the NEF knows no GPSI<->SUPI binding, so
// only GPSI targets match; a subscription naming the UE by IP address only is not reported. NOT
// wired: NEFAFSessionWithQoSNotification (the AF-bound notification is a TS 29.122
// UserPlaneNotificationData since ADR-0520, so it is no longer blocked; not yet built), NIDD,
// device triggering, MSISDN-less MO SMS, parameter and 5G LAN provisioning -- ADR-0487 records why
// each waits.
//
// Disabled unless config/nef.json's li_poi.enabled is true; when disabled main.cpp holds a nullptr.
// Delivery is best-effort: a dead MDF2 is logged, never allowed to break the AF's request.

namespace nef {

class NefLiPoi {
public:
    explicit NefLiPoi(li_poi::Config config);
    ~NefLiPoi();
    NefLiPoi(const NefLiPoi&) = delete;
    NefLiPoi& operator=(const NefLiPoi&) = delete;

    void start();
    void stop();

    // `gpsi` in its TS 29.571 form ("msisdn-..." / "extid-..."); `subscription` (POST/PUT) or
    // `patch` (PATCH) is the AsSessionWithQoSSubscription / ...Patch JSON; `http_status` is the
    // status the NEF returned to the AF. No-op unless a task targets the GPSI.
    void report_af_session_with_qos_provision(const std::string& gpsi,
                                              const std::string& af_id,
                                              li_core::xiri::AfSessionWithQosOpType op,
                                              const std::optional<nlohmann::json>& subscription,
                                              const std::optional<nlohmann::json>& patch,
                                              int http_status);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nef
