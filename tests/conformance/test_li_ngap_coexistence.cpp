// ADR-0364 link proof: one process that uses both the NGAP codec (ngap_generated, static, linked
// flat into the AMF/SMF) and the TS 33.128 codec (li_generated, hidden inside libli_core.so).
// The two ASN.1 modules define 47 identically named types (AllowedNSSAI, AMFPointer, TAIList,
// ...) with different shapes, and each carries its own copy of the asn1c runtime. If li_core's
// symbols ever leaked, this binary would either fail to link or, worse, bind one codec's
// asn_DEF_* descriptor to the other's struct layout at run time. So: encode with both, decode
// with both, and check a type whose name exists in both modules (AMFPointer) is handled by NGAP's
// definition here while li_core encodes its own.
//
// Separate executable, not a case in conformance_tests, because the proof IS the link line.

#include "li_core/x2x3_pdu.hpp"
#include "li_core/xiri.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <AMFPointer.h>
#include <per_encoder.h>
}

TEST(LiNgapCoexistence, BothCodecsWorkInOneProcess) {
    // NGAP's AMFPointer is a BIT STRING (SIZE(6)) -- TS 38.413; TS 33.128's is INTEGER (0..63).
    // Same name, different shape. Use NGAP's here through its own descriptor.
    AMFPointer_t ngap_pointer{};
    std::uint8_t bits = 0xFC; // 6 bits set
    ngap_pointer.buf = &bits;
    ngap_pointer.size = 1;
    ngap_pointer.bits_unused = 2;
    EXPECT_EQ(asn_DEF_AMFPointer.name, std::string("AMFPointer"));
    std::vector<std::uint8_t> out(16);
    asn_enc_rval_t rv =
        aper_encode_to_buffer(&asn_DEF_AMFPointer, nullptr, &ngap_pointer, out.data(), out.size());
    ASSERT_GT(rv.encoded, 0) << "NGAP aper_encode failed";

    // TS 33.128 through li_core only -- no generated header of its own is reachable from here.
    li_core::xiri::AmfRegistration reg;
    reg.supi = li_core::xiri::Imsi{"204081234567890"};
    reg.guti = {"204", "08", 7, 8, 9, 10}; // TS 33.128 AMFPointer = INTEGER 9
    const auto payload = li_core::xiri::encode_xiri_payload(reg);
    ASSERT_TRUE(payload.has_value()) << payload.error();
    const auto back = li_core::xiri::decode_xiri_payload(*payload);
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(std::get<li_core::xiri::AmfRegistration>(back->event).guti.amf_pointer, 9);

    // And the X2 framing.
    const auto wire = li_core::encode(li_core::make_keepalive(1));
    EXPECT_EQ(wire.size(), 48u);
}
