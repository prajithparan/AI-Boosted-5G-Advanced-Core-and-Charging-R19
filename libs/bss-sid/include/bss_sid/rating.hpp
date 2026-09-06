#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

#include "bss_sid/balance.hpp"

// TM Forum SID `AppliedCustomerBillingRate` -> TMF678 Customer Bill Management. Hand-written, not
// codegen'd -- same "no TMF spec vendored, hand-roll it against a real confirmed source" precedent
// as product.hpp/party.hpp/balance.hpp (this file's siblings). Fields confirmed by downloading and
// directly parsing the real TMF678 v4.0.0 swagger JSON
// (github.com/tmforum-apis/TMF678_CustomerBill, TMF678-CustomerBill-v4.0.0.swagger.json), not
// recalled from memory.
//
// Real, disclosed correction to docs/DATA_MODEL.md's own earlier E5 note: that document's original
// field list named `appliedBillingRateType` -- re-fetching the real swagger directly for this ADR
// (2026-08-11, docs/DECISIONS.md ADR-0060) found the real field is simply `type` (a string enum
// documented as `appliedBillingCharge`/`appliedBillingCredit`/`appliedPenaltyCharge`) --
// `appliedBillingRateType` does not exist in the real spec. Corrected here, not silently carried
// forward.
//
// ProductRef/Money/TimePeriod are already modeled (balance.hpp/product.hpp) with an identical real
// shape (confirmed independently against TMF678's own swagger too -- these are TM Forum's shared
// common types), reused directly rather than redefined here.

namespace bss_sid {

struct BillRef {
    std::optional<std::string> id;
    std::optional<std::string> href;
};
void to_json(nlohmann::json& j, const BillRef& v);
void from_json(const nlohmann::json& j, BillRef& v);

struct BillingAccountRef {
    std::optional<std::string> id;
    std::optional<std::string> href;
    std::optional<std::string> name;
};
void to_json(nlohmann::json& j, const BillingAccountRef& v);
void from_json(const nlohmann::json& j, BillingAccountRef& v);

struct AppliedBillingTaxRate {
    std::optional<std::string> taxCategory;
    std::optional<double> taxRate;
    std::optional<Money> taxAmount;
};
void to_json(nlohmann::json& j, const AppliedBillingTaxRate& v);
void from_json(const nlohmann::json& j, AppliedBillingTaxRate& v);

// Real TMF678 AppliedBillingRateCharacteristic -- distinct real type from party.hpp's
// Characteristic/product.hpp's characteristic types (each TMF Open API defines its own copy of
// this common "name/value bag" shape; TMF678's happens to be structurally identical to TMF632's
// Characteristic, but is confirmed and modeled independently per this project's own established
// discipline of not assuming identical shapes across specs without checking).
struct AppliedBillingRateCharacteristic {
    std::optional<std::string> name;
    std::optional<std::string> valueType;
    nlohmann::json value;
};
void to_json(nlohmann::json& j, const AppliedBillingRateCharacteristic& v);
void from_json(const nlohmann::json& j, AppliedBillingRateCharacteristic& v);

// Real TMF678 AppliedCustomerBillingRate -- E5's real SID mapping (docs/DATA_MODEL.md), the
// "rated" representation of a charging decision once TMF654's pre-rating GrantedUnit/AllocatedUnit
// has been converted into a real, TM-Forum-shaped billing rate record. Full real field set.
struct AppliedCustomerBillingRate {
    std::optional<std::string> id;
    std::optional<std::string> href;
    std::optional<std::string> date;
    std::optional<std::string> description;
    std::optional<bool> isBilled;
    std::optional<std::string> name;
    std::optional<std::string>
        type; // appliedBillingCharge | appliedBillingCredit | appliedPenaltyCharge
    std::vector<AppliedBillingTaxRate> appliedTax;
    std::optional<BillRef> bill;
    std::optional<BillingAccountRef> billingAccount;
    std::vector<AppliedBillingRateCharacteristic> characteristic;
    std::optional<TimePeriod> periodCoverage;
    std::optional<ProductRef> product;
    std::optional<Money> taxExcludedAmount;
    std::optional<Money> taxIncludedAmount;
};
void to_json(nlohmann::json& j, const AppliedCustomerBillingRate& v);
void from_json(const nlohmann::json& j, AppliedCustomerBillingRate& v);

// ADR-0310 (C6 of ADR-0300): TMF678 `CustomerBill` -- the bill itself, as distinct from the
// `AppliedCustomerBillingRate` line items above.
//
// Fields taken from the SAME real swagger this file's header already cites
// (github.com/tmforum-apis/TMF678_CustomerBill, TMF678-CustomerBill-v4.0.0.swagger.json), fetched
// and parsed directly for this ADR rather than recalled -- the precedent that already caught one
// wrong field name (`appliedBillingRateType`, which does not exist) is exactly why.
//
// The real definition has NO required fields, so every member here is optional, matching it.
//
// Real, disclosed scope: `appliedPayment`, `billDocument`, `financialAccount`, `paymentMethod`,
// `relatedParty` and `taxItem` are real fields of the spec that this project does not populate --
// it has no payment, document-attachment, financial-account or tax subsystem to source them from.
// They are omitted rather than modelled-and-left-empty, so a consumer cannot mistake an always-
// empty array for "this bill genuinely had no payments".
namespace CustomerBillState {
// Real TMF678 `stateValue` enum, verbatim from the swagger.
inline constexpr const char* kNew = "new";
inline constexpr const char* kOnHold = "onHold";
inline constexpr const char* kValidated = "validated";
inline constexpr const char* kSent = "sent";
inline constexpr const char* kPartiallyPaid = "partiallyPaid";
inline constexpr const char* kSettled = "settled";
} // namespace CustomerBillState

struct CustomerBill {
    std::optional<std::string> id;
    std::optional<std::string> href;
    std::optional<std::string> billDate;
    std::optional<std::string> billNo;
    std::optional<std::string> category;
    std::optional<std::string> lastUpdate;
    std::optional<std::string> nextBillDate;
    std::optional<std::string> paymentDueDate;
    std::optional<std::string> runType;
    std::optional<std::string> state; // CustomerBillState::*
    std::optional<Money> amountDue;
    std::optional<Money> remainingAmount;
    std::optional<Money> taxExcludedAmount;
    std::optional<Money> taxIncludedAmount;
    std::optional<BillingAccountRef> billingAccount;
    std::optional<TimePeriod> billingPeriod;
};
void to_json(nlohmann::json& j, const CustomerBill& v);
void from_json(const nlohmann::json& j, CustomerBill& v);

} // namespace bss_sid
