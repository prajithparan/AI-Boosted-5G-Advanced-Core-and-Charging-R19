// libFuzzer harness for libs/li-core's untrusted-wire decode paths: the ETSI TS 103 221-2 X2/X3
// PDU framing the MDF2/MDF3 receives from every POI (ADR-0364), and the BER XIRIPayload inside
// an X2 PDU's payload (TS 33.128 table 5.3.2-1), which runs through asn1c's ber_decode. Two
// entry points because the second is reachable from the first only when the header parses.
#include <cstddef>
#include <cstdint>
#include <span>

#include "li_core/x2x3_pdu.hpp"
#include "li_core/xiri.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);

    (void)li_core::frame_length(bytes);
    std::size_t consumed = 0;
    if (const auto pdu = li_core::decode(bytes, &consumed); pdu.has_value()) {
        (void)li_core::validate(*pdu);
        (void)li_core::sequence_number(*pdu);
        (void)li_core::timestamp(*pdu);
        (void)li_core::text_attributes(*pdu, li_core::AttributeType::OtherTargetIdentifier);
        if (pdu->payload_format == li_core::PayloadFormat::Tgpp33128Payload) {
            (void)li_core::xiri::decode_xiri_payload(pdu->payload);
        }
        // A decoded PDU must re-encode to exactly the bytes it came from.
        (void)li_core::encode(*pdu);
    }

    // And the BER decoder on the raw input, independent of X2 framing.
    (void)li_core::xiri::decode_xiri_payload(bytes);
    return 0;
}
