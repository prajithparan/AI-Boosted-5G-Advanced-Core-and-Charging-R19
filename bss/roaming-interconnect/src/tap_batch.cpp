#include "tap_batch.hpp"

#include <algorithm>

#include "tap3_core/tap3_charging.hpp"
#include "tap3_core/tap3_gprs_call.hpp"
#include "tap3_core/tap3_mo_call.hpp"

namespace roaming {
namespace {

// TD.57's real TAP 3.12 version pair, the same values libs/tap3-core's own header cites.
constexpr std::int32_t kSpecificationVersionNumber = 3;
constexpr std::int32_t kReleaseVersionNumber = 12;

tap3_core::GprsCall to_gprs_call(const RoamingUsageRecord& record) {
    tap3_core::ChargeableSubscriber subscriber;
    subscriber.isSim = true;
    subscriber.imsi = record.imsi;

    tap3_core::GprsChargeableSubscriber gprs_subscriber;
    gprs_subscriber.chargeableSubscriber = subscriber;

    tap3_core::DateTime start;
    start.localTimeStamp = record.timestamp;
    start.utcTimeOffsetCode = 0;

    tap3_core::GprsBasicCallInformation basic;
    basic.gprsChargeableSubscriber = gprs_subscriber;
    basic.callEventStartTimeStamp = start;

    tap3_core::ChargeDetail detail;
    detail.chargeType = "00"; // real TD.57 "total charge" chargeType
    detail.charge = record.charge;
    detail.chargeableUnits = record.chargeable_units;
    detail.chargedUnits = record.chargeable_units;

    tap3_core::ChargeInformation charge_info;
    charge_info.chargedItem = "X"; // real TD.57 chargedItem for a total/whole-event charge
    charge_info.chargeDetailList.push_back(detail);

    tap3_core::GprsServiceUsed used;
    used.dataVolumeIncoming = record.data_volume_incoming;
    used.dataVolumeOutgoing = record.data_volume_outgoing;
    used.chargeInformationList.push_back(charge_info);

    tap3_core::GprsCall call;
    call.gprsBasicCallInformation = basic;
    call.gprsServiceUsed = used;
    return call;
}

// Sum every ChargeDetail.charge inside one encoded GprsCall.
//
// CallEventDetailList stores each CHOICE alternative as an already-encoded Tlv (an untagged ASN.1
// CHOICE -- see tap3_envelope.hpp), so validating a batch means DECODING the events back out.
// That is deliberate rather than awkward: it means the inbound path exercises the same decoder a
// partner's file goes through, so a batch this project produced and a batch it received are
// checked by identical code.
std::int32_t charge_of_gprs_tlv(const tap3_core::Tlv& tlv) {
    const auto call = tap3_core::decode_gprs_call(tlv);
    if (!call.has_value() || !call->gprsServiceUsed.has_value()) {
        return 0;
    }
    std::int32_t total = 0;
    for (const auto& info : call->gprsServiceUsed->chargeInformationList) {
        for (const auto& detail : info.chargeDetailList) {
            total += detail.charge.value_or(0);
        }
    }
    return total;
}

// The batch's total charge and event count, derived from what the list actually contains.
//
// Real, disclosed narrowing: only `gprsCall` events contribute to the CHARGE total, because that
// is the only variant this project produces and the only one whose charge walk has been verified
// against the spec here. Events of other variants are still COUNTED -- so a partner's file
// carrying voice records is counted correctly and its charge total is reported as
// uncheckable rather than silently computed as zero, which would manufacture a false mismatch.
struct BatchTotals {
    std::int32_t gprs_charge = 0;
    std::size_t event_count = 0;
    std::size_t non_gprs_event_count = 0;
};

BatchTotals totals_of(const tap3_core::CallEventDetailList& list) {
    BatchTotals totals;
    for (const auto& tlv : list.gprsCall) {
        totals.gprs_charge += charge_of_gprs_tlv(tlv);
    }
    totals.event_count = list.gprsCall.size();
    for (const auto* bucket : {&list.mobileOriginatedCall,
                               &list.mobileTerminatedCall,
                               &list.supplServiceEvent,
                               &list.serviceCentreUsage,
                               &list.contentTransaction,
                               &list.locationService,
                               &list.messagingEvent,
                               &list.mobileSession,
                               &list.aggregatedUsageRecord}) {
        totals.event_count += bucket->size();
        totals.non_gprs_event_count += bucket->size();
    }
    return totals;
}

} // namespace

std::string format_file_sequence_number(std::uint32_t value) {
    // Real TD.57 FileSequenceNumber: NumberString(SIZE(5)), so a fixed five characters with
    // leading zeros. It wraps at 99999 by specification -- not an overflow to guard, a documented
    // rollover.
    const std::uint32_t wrapped = value % 100000U;
    std::string out = std::to_string(wrapped);
    out.insert(out.begin(), 5 - out.size(), '0');
    return out;
}

tap3_core::TransferBatch build_transfer_batch(const std::string& sender,
                                              const std::string& recipient,
                                              std::uint32_t file_sequence_number,
                                              const std::string& file_creation_timestamp,
                                              const std::vector<RoamingUsageRecord>& records) {
    tap3_core::BatchControlInfo control;
    control.sender = sender;
    control.recipient = recipient;
    control.fileSequenceNumber = format_file_sequence_number(file_sequence_number);
    tap3_core::DateTimeLong created;
    created.localTimeStamp = file_creation_timestamp;
    created.utcTimeOffset = "+0000";
    control.fileCreationTimeStamp = created;
    control.specificationVersionNumber = kSpecificationVersionNumber;
    control.releaseVersionNumber = kReleaseVersionNumber;

    tap3_core::CallEventDetailList events;
    for (const auto& record : records) {
        events.gprsCall.push_back(tap3_core::encode_gprs_call(to_gprs_call(record)));
    }

    // Every audit total is DERIVED from the events just built. A caller cannot pass a wrong total
    // because it cannot pass one at all -- which is the whole point, since a mismatch here is
    // exactly what gets a file returned by a clearing house.
    tap3_core::AuditControlInfo audit;
    const auto totals = totals_of(events);
    audit.totalCharge = totals.gprs_charge;
    audit.callEventDetailsCount = static_cast<std::int32_t>(totals.event_count);
    if (!records.empty()) {
        const auto [earliest, latest] =
            std::minmax_element(records.begin(),
                                records.end(),
                                [](const RoamingUsageRecord& a, const RoamingUsageRecord& b) {
                                    return a.timestamp < b.timestamp;
                                });
        tap3_core::DateTimeLong first;
        first.localTimeStamp = earliest->timestamp;
        first.utcTimeOffset = earliest->utc_offset;
        tap3_core::DateTimeLong last;
        last.localTimeStamp = latest->timestamp;
        last.utcTimeOffset = latest->utc_offset;
        audit.earliestCallTimeStamp = first;
        audit.latestCallTimeStamp = last;
    }

    tap3_core::TransferBatch batch;
    batch.batchControlInfo = control;
    batch.callEventDetails = events;
    batch.auditControlInfo = audit;
    return batch;
}

std::vector<std::string> validate_transfer_batch(const tap3_core::TransferBatch& batch) {
    std::vector<std::string> findings;

    if (!batch.batchControlInfo.has_value()) {
        findings.push_back("missing BatchControlInfo");
    } else {
        const auto& control = *batch.batchControlInfo;
        if (!control.sender.has_value() || control.sender->empty()) {
            findings.push_back("BatchControlInfo has no sender");
        }
        if (!control.recipient.has_value() || control.recipient->empty()) {
            findings.push_back("BatchControlInfo has no recipient");
        }
        // A gap-free 5-character sequence number is how a recipient detects a lost file, so a
        // malformed one is a real finding rather than cosmetic.
        if (!control.fileSequenceNumber.has_value() || control.fileSequenceNumber->size() != 5) {
            findings.push_back("BatchControlInfo fileSequenceNumber is not 5 characters");
        }
    }

    if (!batch.auditControlInfo.has_value()) {
        findings.push_back("missing AuditControlInfo");
        return findings; // nothing left to cross-check against
    }

    const auto& audit = *batch.auditControlInfo;
    const auto totals =
        batch.callEventDetails.has_value() ? totals_of(*batch.callEventDetails) : BatchTotals{};
    const std::size_t actual_count = totals.event_count;
    if (!audit.callEventDetailsCount.has_value()) {
        findings.push_back("AuditControlInfo has no callEventDetailsCount");
    } else if (static_cast<std::size_t>(*audit.callEventDetailsCount) != actual_count) {
        findings.push_back(
            "callEventDetailsCount says " + std::to_string(*audit.callEventDetailsCount) +
            " but the batch carries " + std::to_string(actual_count) + " call events");
    }

    if (!audit.totalCharge.has_value()) {
        findings.push_back("AuditControlInfo has no totalCharge");
    } else if (totals.non_gprs_event_count > 0) {
        // Honest abstention rather than a false mismatch: this module can only walk GPRS charges,
        // so a batch containing other variants has a total it cannot verify. Saying so is the
        // correct outcome -- computing those as zero would report every voice-bearing partner file
        // as broken.
        findings.push_back("totalCharge not verified: batch carries " +
                           std::to_string(totals.non_gprs_event_count) +
                           " non-GPRS call events whose charges this validator does not walk");
    } else if (*audit.totalCharge != totals.gprs_charge) {
        findings.push_back("totalCharge says " + std::to_string(*audit.totalCharge) +
                           " but the call events sum to " + std::to_string(totals.gprs_charge));
    }

    return findings;
}

std::vector<std::string> validate_tap_file(const std::vector<std::uint8_t>& bytes) {
    const auto interchange = tap3_core::decode_data_interchange(bytes);
    if (!interchange.has_value()) {
        return {"file is not a decodable TAP3 DataInterchange"};
    }
    if (!interchange->transferBatch.has_value()) {
        // A Notification is a real, valid interchange -- "no chargeable data this period" -- so it
        // is not a finding, it simply has no batch to cross-check.
        if (interchange->notification.has_value()) {
            return {};
        }
        return {"DataInterchange carries neither a TransferBatch nor a Notification"};
    }
    return validate_transfer_batch(*interchange->transferBatch);
}

} // namespace roaming
