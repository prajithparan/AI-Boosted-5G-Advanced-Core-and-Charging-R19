#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tap3_core/tap3_envelope.hpp"

// ADR-0306: TAP OUT and TAP IN batch processing -- the settlement half of ADR-0300's C5.
//
// `libs/tap3-core` already implements GSMA TD.57 TAP 3.12 in both directions (112 encode/decode
// functions, all nine CallEventDetail variants). What did not exist -- and what
// `store.hpp` disclosed in its own comment -- is anything that POPULATES a DataInterchange from
// this project's own roaming usage, or that VALIDATES one a partner sent. A codec is not a
// settlement path.
//
// The property this module exists to hold is the one that gets real files REJECTED by a clearing
// house: `AuditControlInfo` must agree with the call events actually present. A batch whose
// `callEventDetailsCount` or `totalCharge` disagrees with its own contents is returned via RAP,
// which costs an operator a settlement cycle. So the totals are DERIVED from the records here,
// never passed in -- a caller cannot supply a wrong total because it cannot supply one at all --
// and the inbound direction recomputes them and reports every disagreement.

namespace roaming {

// One rated roaming usage record, in this project's own terms rather than TAP3's. Deliberately
// small: it carries exactly what is needed to produce a real GprsCall and to compute audit totals,
// so a caller assembling these from CHF's CDRs does not need to understand TAP3's structure.
struct RoamingUsageRecord {
    std::string imsi;
    std::int64_t data_volume_incoming = 0; // octets
    std::int64_t data_volume_outgoing = 0; // octets
    // Charge in the batch's own currency units, as TAP3 carries it: an integer in the smallest
    // unit, scaled by the file's TAP decimal places. No floating point anywhere near settlement.
    std::int32_t charge = 0;
    std::int32_t chargeable_units = 0;
    // TAP3 DateTimeLong: "YYYYMMDDHHMMSS" plus a UTC offset like "+0000".
    std::string timestamp;
    std::string utc_offset = "+0000";
};

// TAP OUT: build a complete, self-consistent TransferBatch.
//
// `file_sequence_number` is the operator's own monotonically increasing counter per
// sender/recipient pair -- TD.57 requires it to be gap-free, because a gap is what tells a
// recipient a file was lost. It is a caller responsibility (it must survive restarts, so it cannot
// live here), and this function formats it to the real 5-character fixed width the spec requires.
tap3_core::TransferBatch build_transfer_batch(const std::string& sender,
                                              const std::string& recipient,
                                              std::uint32_t file_sequence_number,
                                              const std::string& file_creation_timestamp,
                                              const std::vector<RoamingUsageRecord>& records);

// The real 5-character fixed-width TAP file sequence number ("00001".."99999"), exposed because a
// caller persisting the counter needs to render it the same way.
std::string format_file_sequence_number(std::uint32_t value);

// TAP IN: what is wrong with a batch a partner sent, as a list of human-readable findings.
// Empty means the batch is internally consistent. This checks the properties a clearing house
// checks -- presence of the control blocks, and agreement between AuditControlInfo and the actual
// call events -- not the whole of TD.57's validation rule set, which is far larger and is NOT
// claimed here.
std::vector<std::string> validate_transfer_batch(const tap3_core::TransferBatch& batch);

// Convenience: decode a partner's file and validate it in one step. A file that does not decode at
// all reports that as its single finding rather than throwing.
std::vector<std::string> validate_tap_file(const std::vector<std::uint8_t>& bytes);

} // namespace roaming
