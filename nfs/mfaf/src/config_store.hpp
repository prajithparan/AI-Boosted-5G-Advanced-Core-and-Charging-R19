#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <utility>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29576_Nmfaf_3caDataManagement.hpp"
#include "TS29576_Nmfaf_3daDataManagement.hpp"

// MFAF state in Valkey (ADR-0365, the ADR-0359 rule: no in-process state in the NWDAF ecosystem).
//
//   mfaf:cfg:<transRefId>        JSON MfafConfiguration -- the "Individual MFAF Configuration"
//   mfaf:cfgs                    SET of transRefIds (the collection, for transfer and audit)
//   mfaf:corr:<mfafCorreId>      transRefId -- the reverse index an inbound notification needs:
//                                the Data Source only knows the MFAF Notification Correlation ID
//   mfaf:buf:<fetchCorrId>       JSON NmfafDataAnaNotification buffered for a consumer that asked
//                                for consumer-triggered delivery (consTrigNotif), with the expiry
//                                the FetchInstruction promised as its TTL
//   mfaf:bufidx:<transRefId>     SET of fetchCorrIds buffered for that configuration (transfer
//                                hands them over as "bufferedNotifs")
//   mfaf:next_id                 INCR counter behind every id, unique across replicas
//
// Every replica reads and writes the same keys, so a configuration created on one replica is
// deconfigured on another, a fetch lands on whichever replica the consumer reaches, and a
// transfer can hand over buffers a different replica filled.

namespace mfaf {

class ConfigStore {
public:
    explicit ConfigStore(std::shared_ptr<sw::redis::Redis> redis) : redis_(std::move(redis)) {}

    // Returns the new transRefId ("mfaf-cfg-<n>"). Indexes every mfafCorreId in the configuration.
    std::string create(const sbi_gen::MfafConfiguration& cfg);
    std::optional<sbi_gen::MfafConfiguration> get(const std::string& id);
    // false when no such configuration exists (PUT is then a 404, never an upsert).
    bool replace(const std::string& id, const sbi_gen::MfafConfiguration& cfg);
    bool remove(const std::string& id);
    std::optional<std::pair<std::string, sbi_gen::MfafConfiguration>>
    find_by_correlation(const std::string& mfaf_corre_id);

    // Consumer-triggered delivery buffers.
    std::string buffer_put(const std::string& trans_ref_id,
                           const sbi_gen::NmfafDataAnaNotification& notification,
                           std::chrono::seconds ttl);
    // GETDEL: a fetch consumes the buffer. nullopt when unknown or expired.
    std::optional<sbi_gen::NmfafDataAnaNotification> buffer_take(const std::string& fetch_corr_id);
    std::vector<std::pair<std::string, sbi_gen::NmfafDataAnaNotification>>
    buffers_for(const std::string& trans_ref_id);

    std::string next_id(const char* prefix);

private:
    void index(const std::string& id, const sbi_gen::MfafConfiguration& cfg);
    void unindex(const sbi_gen::MfafConfiguration& cfg);
    std::shared_ptr<sw::redis::Redis> redis_;
};

} // namespace mfaf
