// ADR-0353: a CAMEL call rates against the offering scoped to its serviceKey -- end to end.
//
// ADR-0351 found that cap_attributes() was built to make CAP traffic productisable and then never
// called from anywhere but its own unit test, so every CAMEL call passed no attributes at all and
// could only match an UNSCOPED offering. It was fixed, and that ADR recorded plainly that the fix
// was not proven end to end, because the test to prove it needed ports the 3M-CDR soak was using.
// This is that test.
//
// Why it is built the way it is:
//
//   * TWO offerings are seeded with the SAME ratingGroup, differing ONLY in their chargingScope
//     (CAP.serviceKey 4242 vs 9999). Rating group therefore cannot pick the winner -- only the CAP
//     attributes can. That is the entire point: with the ADR-0351 bug present, the attribute map
//     is empty, NEITHER scope matches, no offering is selected and NO rating decision is written.
//     So the assertion "a decision exists naming the 4242 price" fails on the original bug rather
//     than merely passing on the fix.
//
//   * The assertion reads the RatingDecision audit row, not CHF's log. A log line proves a code
//     path was entered; the audit row is what an operator would actually be billed from, and it
//     carries the offering and price that were chosen.
//
//   * product-catalog and balance-management are spawned. test_chf_protocol_ceilings.cpp drives an
//     InitialDP over this same M3UA/SCTP harness but deliberately spawns neither, which is exactly
//     why it can prove "dispatched vs shed" and cannot prove anything about rating.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <pqxx/pqxx>

#include "../../bss/product-catalog/src/store.hpp"
#include "cap_core/cap_dictionary.hpp"
#include "cap_core/cap_operations.hpp"
#include "spawn_guard.hpp"
#include "ss7_core/m3ua_asp.hpp"
#include "ss7_core/m3ua_dictionary.hpp"
#include "ss7_core/m3ua_header.hpp"
#include "ss7_core/m3ua_protocol_data.hpp"
#include "ss7_core/m3ua_tlv.hpp"
#include "ss7_core/sccp_dictionary.hpp"
#include "ss7_core/sccp_udt.hpp"
#include "ss7_core/sctp_socket.hpp"
#include "tbcd_core/tbcd.hpp"
#include "tcap_core/component.hpp"
#include "tcap_core/dialogue_portion.hpp"
#include "tcap_core/message.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

// Deliberately far from the 1..11 rating groups the lab catalog seeds, so this test cannot be
// satisfied by, or collide with, an offering it did not create.
constexpr std::int32_t kServiceKey = 4242;
constexpr std::int32_t kOtherServiceKey = 9999;
constexpr const char* kImsiDigits = "999700000424242";

std::string catalog_conninfo() {
    if (const char* env = std::getenv("TEST_POSTGRES_URL")) {
        return env;
    }
    return "postgresql://product_catalog:product_catalog@localhost:5432/product_catalog";
}

std::string rating_conninfo() {
    if (const char* env = std::getenv("CHF_RATING_DATABASE_URL")) {
        return env;
    }
    return "postgresql://postgres@127.0.0.1:5434/chf_rating";
}

std::string log_file(const std::string& tag) {
    return (std::filesystem::temp_directory_path() / ("cap-scope-" + tag + ".log")).string();
}

bool wait_for_line(const std::string& path, const std::string& needle, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

// One offering + one price, linked, with a ratingGroup and a CAP-scoped chargingScope. Returns the
// price's name, which is what the RatingDecision audit row records.
struct SeededOffering {
    std::string price_name;
    std::string price_id;
    std::string offering_id;
};

SeededOffering seed_scoped_offering(product_catalog::ProductOfferingStore& offerings,
                                    product_catalog::ProductOfferingPriceStore& prices,
                                    const std::string& label,
                                    std::int32_t scoped_service_key) {
    const std::string price_name = "CAP Scope Test " + label;

    bss_sid::ProductOfferingPrice price{};
    price.name = price_name;
    price.priceType = "usage";
    price.lifecycleStatus = "Active";
    // Without these CHF matches the price and then grants nothing ("has no unitOfMeasure,
    // granting nothing"), so no RatingDecision is written and this test cannot tell a scoping
    // failure from an incomplete fixture. Found exactly that way.
    bss_sid::Quantity uom;
    uom.units = "GB";
    uom.amount = 10.0;
    price.unitOfMeasure = uom;
    bss_sid::Money money;
    money.unit = "EUR";
    money.value = 10.0;
    price.price = money;

    bss_sid::ProductSpecificationCharacteristicValueUse rg;
    rg.id = "rg-" + label;
    rg.name = "ratingGroup";
    bss_sid::CharacteristicValueSpecification rg_value;
    rg_value.value = json(kServiceKey);
    rg.productSpecCharacteristicValue.push_back(rg_value);
    price.prodSpecCharValueUse.push_back(rg);

    bss_sid::ProductSpecificationCharacteristicValueUse scope;
    scope.id = "sc-" + label;
    scope.name = "chargingScope";
    bss_sid::CharacteristicValueSpecification scope_value;
    // The attribute key cap_attributes() emits, and an INTEGER because serviceKey is an ASN.1
    // INTEGER -- a string here would silently never match, which is the failure mode the
    // scoping code comments warn about.
    scope_value.value = json{{"CAP.serviceKey", scoped_service_key}};
    scope.productSpecCharacteristicValue.push_back(scope_value);
    price.prodSpecCharValueUse.push_back(scope);

    const auto price_id = prices.create(price);

    bss_sid::ProductOffering offering{};
    offering.name = "CAP Scope Offering " + label;
    offering.lifecycleStatus = "Active";
    offering.isSellable = true;
    bss_sid::ProductOfferingPriceRef ref;
    ref.id = price_id;
    offering.productOfferingPrice.push_back(ref);
    const auto offering_id = offerings.create(offering);

    return SeededOffering{price_name, price_id, offering_id};
}

void send_m3ua(ss7_core::SctpSocket& sock,
               std::uint8_t cls,
               std::uint8_t type,
               const std::vector<std::uint8_t>& body) {
    auto msg = ss7_core::encode_m3ua_header({cls, type}, static_cast<std::uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    sock.send(msg);
}

// A real InitialDP carrying an IMSI -- unlike the ceilings test's, which omits it deliberately
// because that test never needs CHF to get as far as charging. Here it must.
std::vector<std::uint8_t> build_initial_dp(std::int32_t service_key) {
    cap_core::InitialDpArg idp;
    idp.service_key = service_key;
    idp.called_party_number = {0x00, 0x11, 0x22};
    idp.calling_party_number = std::vector<std::uint8_t>{0x00, 0x33, 0x44};
    idp.event_type_bcsm = 12; // collectedInfo
    idp.imsi = tbcd_core::encode_tbcd(kImsiDigits);

    tcap_core::Invoke invoke;
    invoke.invoke_id = 1;
    invoke.operation_code.local = cap_core::Opcode::kInitialDp;
    invoke.parameter = cap_core::encode_initial_dp_arg(idp);

    tcap_core::DialogueRequest aarq;
    aarq.application_context_name = cap_core::kGsmssfScfGenericAcOid;
    tcap_core::TcBegin begin;
    begin.originating_transaction_id = {0x00, 0x00, 0x04, 0x02};
    begin.dialogue_portion = tcap_core::encode_dialogue_portion_request(aarq);
    begin.components.push_back(tcap_core::encode_invoke(invoke));

    ss7_core::SccpUdt udt;
    udt.protocol_class = ss7_core::dictionary::ProtocolClass::kClass0;
    udt.called_party.ssn_present = true;
    udt.called_party.ssn = ss7_core::dictionary::SubsystemNumber::kHlr;
    udt.calling_party.ssn_present = true;
    udt.calling_party.ssn = ss7_core::dictionary::SubsystemNumber::kMsc;
    udt.data = tcap_core::encode_tc_begin(begin);

    ss7_core::M3uaProtocolData pd;
    pd.opc = 1;
    pd.dpc = 2;
    pd.si = ss7_core::dictionary::ServiceIndicator::kSccp;
    pd.ni = 2;
    pd.sls = 0;
    pd.user_protocol_data = ss7_core::encode_sccp_udt(udt);
    ss7_core::M3uaTlv pd_tlv;
    pd_tlv.tag = ss7_core::dictionary::ParamTag::kProtocolData;
    pd_tlv.value = ss7_core::encode_m3ua_protocol_data(pd);
    std::vector<std::uint8_t> out;
    ss7_core::encode_m3ua_tlv(out, pd_tlv);
    return out;
}

// Rating decisions naming this price, written since the test started.
int decisions_for_price(const std::string& price_name, const std::string& since) {
    try {
        pqxx::connection conn(rating_conninfo());
        pqxx::work tx(conn);
        const auto row = tx.exec_params1(
            "SELECT COUNT(*) FROM rating_decision "
            "WHERE input_snapshot->>'priceName' = $1 AND decided_at >= $2::timestamptz",
            price_name, since);
        return row[0].as<int>();
    } catch (const std::exception&) {
        return -1; // unreachable store -- reported as a distinct failure, never as "no decisions"
    }
}

std::string now_iso() {
    pqxx::connection conn(rating_conninfo());
    pqxx::work tx(conn);
    return tx.exec1("SELECT now()::text")[0].as<std::string>();
}

} // namespace

TEST(CapScopedCharging, AnInitialDpRatesAgainstTheOfferingScopedToItsServiceKey) {
    ASSERT_NO_THROW({ pqxx::connection probe(rating_conninfo()); })
        << "the RatingDecision store must be reachable, or this test proves nothing";
    const std::string started_at = now_iso();

    product_catalog::ProductOfferingStore offerings(
        "https://test/tmf-api/productCatalogManagement/v4/productOffering", catalog_conninfo());
    product_catalog::ProductOfferingPriceStore prices(
        "https://test/tmf-api/productCatalogManagement/v4/productOfferingPrice",
        catalog_conninfo());

    // The decoy is seeded FIRST so it is encountered first in the catalog listing: if CAP
    // attributes were ignored, whichever offering came first would win, and the decoy winning is
    // as much a failure as nothing winning.
    const auto decoy = seed_scoped_offering(offerings, prices, "Decoy", kOtherServiceKey);
    const auto wanted = seed_scoped_offering(offerings, prices, "Wanted", kServiceKey);
    const auto& decoy_price = decoy.price_name;
    const auto& wanted_price = wanted.price_name;

    const std::string chf_log = log_file("chf");
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess catalog(PRODUCT_CATALOG_PATH);
    nf_test::SpawnedProcess balance(BALANCE_MANAGEMENT_PATH);
    nf_test::SpawnedProcess chf(CHF_PATH, chf_log.c_str());
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(catalog.pid(), 0);
    ASSERT_GT(balance.pid(), 0);
    ASSERT_GT(chf.pid(), 0);
    ASSERT_TRUE(wait_for_line(chf_log, "CAP (gsmSCF) listening", 30s)) << "CHF never opened CAP";

    ss7_core::SctpSocket sock;
    sock.connect("127.0.0.1", ss7_core::dictionary::kSctpPort);
    send_m3ua(
        sock,
        ss7_core::dictionary::MessageClass::kAspsm,
        ss7_core::dictionary::AspsmMessageType::kAspUp,
        ss7_core::encode_asp_state_message(ss7_core::dictionary::AspsmMessageType::kAspUp, {}));
    ASSERT_FALSE(sock.receive().empty()) << "no ASP Up Ack";
    ss7_core::AspTrafficMessage active;
    active.traffic_mode_type = ss7_core::dictionary::TrafficModeType::kOverride;
    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kAsptm,
              ss7_core::dictionary::AsptmMessageType::kAspActive,
              ss7_core::encode_asp_traffic_message(
                  ss7_core::dictionary::AsptmMessageType::kAspActive, active));
    ASSERT_FALSE(sock.receive().empty()) << "no ASP Active Ack";

    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kTransfer,
              ss7_core::dictionary::TransferMessageType::kData,
              build_initial_dp(kServiceKey));

    ASSERT_TRUE(wait_for_line(chf_log, "real CAP InitialDP received", 20s))
        << "CHF never reported receiving the InitialDP";

    // The rating decision is written on the charging path, which runs after the InitialDP is
    // parsed; poll rather than assume it has landed by the time the log line appears.
    int wanted_decisions = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        wanted_decisions = decisions_for_price(wanted_price, started_at);
        if (wanted_decisions > 0) {
            break;
        }
        std::this_thread::sleep_for(250ms);
    }

    EXPECT_GT(wanted_decisions, 0)
        << "no RatingDecision named the serviceKey-scoped price. With ADR-0351's bug present the "
           "CAP path passes no attributes, so neither scoped offering matches and nothing is "
           "rated -- which is exactly this outcome.";
    EXPECT_EQ(decisions_for_price(decoy_price, started_at), 0)
        << "the decoy offering (scoped to a DIFFERENT serviceKey) was rated, so chargingScope did "
           "not actually discriminate on the CAP attributes";

    // Keep the log when the test fails -- it is the only record of what CHF actually did, and
    // deleting it unconditionally is how a failure becomes undiagnosable.
    // The catalog is shared lab state; leaving fixtures behind makes every later run start from a
    // different catalog than this one did.
    offerings.remove(wanted.offering_id);
    offerings.remove(decoy.offering_id);
    prices.remove(wanted.price_id);
    prices.remove(decoy.price_id);

    if (!::testing::Test::HasFailure()) {
        std::error_code rm;
        std::filesystem::remove(chf_log, rm);
    } else {
        std::cerr << "CHF log preserved for diagnosis: " << chf_log << "\n";
    }
}
