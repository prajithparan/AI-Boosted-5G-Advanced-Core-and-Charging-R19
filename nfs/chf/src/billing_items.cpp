#include "billing_items.hpp"

#include "sbi_core/datetime.hpp"

namespace chf {

std::vector<bss_sid::AppliedCustomerBillingRate>
cdrs_to_billing_items(const std::vector<CdrRecord>& cdrs) {
    std::vector<bss_sid::AppliedCustomerBillingRate> items;
    items.reserve(cdrs.size());

    for (const auto& cdr : cdrs) {
        if (!cdr.reserved_cost.has_value()) {
            // Nothing was committed against the balance for this row -- see billing_items.hpp.
            continue;
        }

        bss_sid::AppliedCustomerBillingRate item{};
        item.id = cdr.charging_data_ref + "-" + std::to_string(cdr.invocation_sequence_number);
        item.name = "Data usage";
        item.type = "appliedBillingCharge";
        item.isBilled = false;
        item.date = sbi_core::format_rfc3339(
            std::chrono::system_clock::from_time_t(cdr.invocation_time_stamp));

        bss_sid::Money amount{};
        amount.value = *cdr.reserved_cost;
        amount.unit = cdr.reserved_cost_currency;
        item.taxExcludedAmount = amount;
        item.taxIncludedAmount = amount; // no tax engine -- see billing_items.hpp

        bss_sid::TimePeriod period{};
        period.startDateTime = item.date;
        period.endDateTime = item.date;
        item.periodCoverage = period;

        // Traceability: a disputed charge must lead back to the session that produced it.
        bss_sid::AppliedBillingRateCharacteristic ref_char{};
        ref_char.name = "chargingDataRef";
        ref_char.valueType = "string";
        ref_char.value = cdr.charging_data_ref;
        item.characteristic.push_back(ref_char);

        if (cdr.rating_group.has_value()) {
            bss_sid::AppliedBillingRateCharacteristic rg_char{};
            rg_char.name = "ratingGroup";
            rg_char.valueType = "number";
            rg_char.value = *cdr.rating_group;
            item.characteristic.push_back(rg_char);
        }
        if (cdr.used_total_volume.has_value()) {
            bss_sid::AppliedBillingRateCharacteristic vol_char{};
            vol_char.name = "usedTotalVolume";
            vol_char.valueType = "number";
            vol_char.value = *cdr.used_total_volume;
            item.characteristic.push_back(vol_char);
        }
        // ADR-0311: roaming usage is marked on the line item, so a bill can show it separately
        // without re-querying. Only set when true -- a `false` characteristic on every domestic
        // charge is noise that says nothing.
        if (cdr.is_roaming) {
            bss_sid::AppliedBillingRateCharacteristic roam_char{};
            roam_char.name = "roaming";
            roam_char.valueType = "boolean";
            roam_char.value = true;
            item.characteristic.push_back(roam_char);
            if (!cdr.serving_plmn.empty()) {
                bss_sid::AppliedBillingRateCharacteristic plmn_char{};
                plmn_char.name = "servingPlmn";
                plmn_char.valueType = "string";
                plmn_char.value = cdr.serving_plmn;
                item.characteristic.push_back(plmn_char);
            }
            item.name = "Roaming data usage";
        }

        items.push_back(std::move(item));
    }
    return items;
}

} // namespace chf
