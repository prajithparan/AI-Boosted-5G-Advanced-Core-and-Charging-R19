#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include "bss_sid/agreement.hpp"
#include "tap3_core/tap3_envelope.hpp"

// Private to bss/roaming-interconnect. Real PostgreSQL persistence (libpqxx), same "one shared
// connection, one mutex" discipline this project already applied to every other bss/* store --
// see schema.sql's own header for the full scoping disclosure (schema + store library only this
// turn, no new HTTP service yet -- CHARGING_PROMPT.md's P4.11 owns that).

namespace roaming_interconnect {

// ADR-0387: a malformed date-time or a reference the relational model rejects is a 400.
class InvalidRequest : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// E7 InterconnectAgreement -- project-internal, realized as a real TMF651 Agreement
// (bss_sid::Agreement) plus the two project-internal fields (partnerOperatorPlmnId, rateTerms)
// docs/DATA_MODEL.md's own sketch names, neither of which has a real TMF651 field home.
struct InterconnectAgreement {
    std::optional<std::string> id;
    std::optional<std::string> href;
    std::optional<std::string> partnerOperatorPlmnId;
    bss_sid::Agreement agreement;
    nlohmann::json rateTerms; // opaque -- "partner-specific rating; shape TBD, not guessed"
};

void to_json(nlohmann::json& j, const InterconnectAgreement& v);
void from_json(const nlohmann::json& j, InterconnectAgreement& v);

// E7 RoamingCdrFile -- format "TAP3" is now real (libs/tap3-core, ADR-0067); RAP/NRTRDE remain
// real-spec-disclosed as STUB until their specs are supplied (schema.sql's own header).
struct RoamingCdrFile {
    std::optional<std::string> id;
    std::optional<std::string> agreementId;
    std::string format = "STUB";       // TAP3 | RAP | NRTRDE | STUB
    std::vector<std::byte> rawPayload; // matches libpqxx's own real bytea representation
                                       // (pqxx::bytes = std::vector<std::byte>) directly, no cast
};

// Real TAP3 wiring (ADR-0067): builds a RoamingCdrFile with format="TAP3" and rawPayload set to a
// real BER-encoded DataInterchange, and the reverse decode. Real, disclosed gap: what actually
// POPULATES a real DataInterchange from this project's own live CDR data (a CHF-triggered rating
// event) is separate, later wiring, not this function -- these two functions only prove the file
// carries a real, tag-correct TAP3 byte stream, matching the same "codec proven byte-correct, live
// data path deferred" scope disclosed throughout libs/tap3-core.
RoamingCdrFile make_tap3_roaming_cdr_file(std::optional<std::string> agreementId,
                                          const tap3_core::DataInterchange& data);
std::optional<tap3_core::DataInterchange> decode_tap3_roaming_cdr_file(const RoamingCdrFile& file);

class InterconnectAgreementStore {
public:
    InterconnectAgreementStore(std::string resource_url, const std::string& conninfo);

    std::string create(InterconnectAgreement agreement);
    std::optional<InterconnectAgreement> get(const std::string& id);
    std::vector<InterconnectAgreement> list();

private:
    std::string resource_url_;
    std::mutex mutex_;
    pqxx::connection conn_;
};

class RoamingCdrFileStore {
public:
    explicit RoamingCdrFileStore(const std::string& conninfo);

    std::string create(RoamingCdrFile file);
    std::optional<RoamingCdrFile> get(const std::string& id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

} // namespace roaming_interconnect
