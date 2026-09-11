#pragma once

// ADR-0344: which of TS 32.291's charging-information blocks a request carries.
//
// The specification defines TWENTY-FIVE of them. CHF parsed exactly one --
// `pDUSessionChargingInformation` -- and silently dropped the other twenty-four: an SMS, an MMTel
// call, an MBS session and a PDU session all produced byte-identical CDRs apart from rating group.
// The charging system could accept an SMS charging request and lose every fact that made it an SMS.
//
// That is a capability gap against any production charging system, not a cosmetic one: an operator
// cannot bill, rate or reconcile a service whose records do not say what service they are.
//
// This detects the block that is present, names it, and preserves it. Detection is exhaustive by
// construction -- every field the generated DTO carries is checked -- so a charging-information
// type added in a future 3GPP release surfaces as a compile error here rather than as silently
// discarded revenue.

#include <nlohmann/json.hpp>

#include <string>

namespace sbi_gen {
struct ChargingDataRequest_Nchf_ConvergedCharging;
}

namespace chf {

struct DetectedChargingInformation {
    // "PDUSession", "SMS", "MMTel", ... Empty when the request carries no block, which is a real
    // state rather than an error.
    std::string type;
    // The block itself, preserved as JSON so no field is lost to a column that does not exist.
    nlohmann::json payload = nlohmann::json::object();

    bool present() const { return !type.empty(); }
};

// TS 32.291 permits several blocks in one request (an interCHF record alongside a service block,
// for instance). The FIRST service-bearing block in specification order wins as the record's type,
// and every block present is preserved in the payload under its own key -- so a multi-block
// request loses nothing.
DetectedChargingInformation
detect_charging_information(const sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging& request);

// ADR-0345: flatten any charging-information block into dotted attribute paths, so an operator can
// price on ANY field the specification defines without a code change here.
//
// The rating engine matches a product's `chargingScope` against a flat attribute map. Until now
// that map was populated by hand, field by field, from one block -- so an operator could scope on
// `dnnId` because someone had written a line for it, and could not scope on an SMS message type,
// an MMTel supplementary service or an MBS session id at all, because nobody had.
//
// That is the difference between a charging system that can be productised and one that needs an
// engineering change per tariff. Flattening inverts it: every scalar in the block becomes
// `<Type>.<path>` (e.g. "SMS.originatorInfo.originatorSUPI", "MBSSession.mbsSessionId"), and a new
// 3GPP field is scopable the day the DTO carries it.
//
// Arrays are indexed ("recipientInfo.0.recipientSUPI") rather than collapsed: a product priced on
// the first recipient of a multi-recipient MMS is a real tariff, and flattening that away would
// silently make it unexpressible.
//
// `max_depth` bounds the walk. A pathological or recursive document cannot be allowed to turn one
// charging request into unbounded work on the hot path.
void flatten_attributes(const nlohmann::json& value,
                        const std::string& prefix,
                        nlohmann::json& out,
                        int max_depth = 6);

} // namespace chf
