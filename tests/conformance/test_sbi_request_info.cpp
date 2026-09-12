// TS 29.500 clause 5.2.3.3.12 (3gpp-Sbi-Request-Info) and clause 5.2.8 (detection of duplicated
// request message), against the real ETSI TS 129 500 V19.7.0 (2026-08) text at
// specs/TS_29_500.pdf.
//
// The header had a constant in sbi_headers.hpp and nothing that parsed it. It is parsed now
// because CHF uses the idempotency-key to answer a retransmitted charging request with the
// response the original got, instead of a 404 that is indistinguishable from a genuinely unknown
// ChargingDataRef.

#include "sbi_core/sbi_headers.hpp"

#include <gtest/gtest.h>

namespace {

using sbi_core::headers::parse_request_info;
using sbi_core::headers::request_info_idempotency_key;

TEST(SbiRequestInfo, ParsesTheSpecsOwnExamples) {
    // EXAMPLE 1 from clause 5.2.3.3.12, verbatim.
    const auto ex1 = parse_request_info(
        "retrans=true; redirect=true; reason=temporary-rejection-cause; "
        "receivedrejectioncause=INSUFFICIENT_RESOURCES");
    EXPECT_EQ(ex1.at("retrans"), "true");
    EXPECT_EQ(ex1.at("redirect"), "true");
    EXPECT_EQ(ex1.at("reason"), "temporary-rejection-cause");
    EXPECT_EQ(ex1.at("receivedrejectioncause"), "INSUFFICIENT_RESOURCES");

    // EXAMPLE 3, the non-idempotent-request case this exists for.
    EXPECT_EQ(request_info_idempotency_key(
                  "idempotency-key=54804518-4191-46b3-955c-ac631f953ed8"),
              "54804518-4191-46b3-955c-ac631f953ed8");

    // EXAMPLE 4 quotes its value; the quotes are not part of it.
    EXPECT_EQ(parse_request_info("callback-uri-prefix=\"/abc\"").at("callback-uri-prefix"), "/abc");
}

TEST(SbiRequestInfo, HonoursTheOptionalWhitespaceTheAbnfAllows) {
    // req-param = req-param-name "=" OWS req-param-value, and the list separator is ";" OWS.
    // Whitespace left in the value would make a key compare unequal to the same key on the retry,
    // which is the one comparison this header exists to make.
    const auto key = request_info_idempotency_key("retrans=true;   idempotency-key=  abc-123  ");
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, "abc-123");
}

TEST(SbiRequestInfo, AnUnknownParameterDoesNotInvalidateTheHeader) {
    // req-param-name includes bare `token`, so a parameter this build has never heard of is legal
    // and must not cost us the ones we do understand.
    const auto params = parse_request_info("some-future-param=x; idempotency-key=k1");
    EXPECT_EQ(params.at("idempotency-key"), "k1");
    EXPECT_EQ(params.at("some-future-param"), "x");
}

TEST(SbiRequestInfo, NoKeyIsAbsentRatherThanEmpty) {
    // Clause 5.2.8 makes the whole mechanism optional for clients, so "no key" is the normal case
    // and must be distinguishable from an empty one -- an empty key would otherwise become a
    // cache entry every keyless request collided on.
    EXPECT_FALSE(request_info_idempotency_key("retrans=true").has_value());
    EXPECT_FALSE(request_info_idempotency_key("").has_value());
    EXPECT_FALSE(request_info_idempotency_key("idempotency-key=").has_value());
    EXPECT_FALSE(request_info_idempotency_key("garbage-without-equals").has_value());
}

} // namespace
