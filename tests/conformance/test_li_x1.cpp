// ETSI TS 103 221-1 V1.23.1 LI_X1 codec + NE server + keepalive machine (ADR-0372). Assertions
// go against the spec: round-trips validate against the committed XSD, error codes match
// table 6.7-3, keepalive transitions match clause 6.6.2. No transport here (the X2/X3 client is
// exercised by its own integration path); this is the codec floor.

#include <chrono>
#include <string>

#include "li_core/x1.hpp"
#include "li_core/x1_server.hpp"

#include <gtest/gtest.h>

namespace {

using namespace li_core::x1;
using namespace std::chrono_literals;

std::string envelope(const std::string& inner_type, const std::string& inner_body) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <x1RequestMessage xsi:type=")" +
           inner_type + R"(">
    <admfIdentifier>admf-1</admfIdentifier>
    <neIdentifier>amf-1</neIdentifier>
    <messageTimestamp>2026-09-16T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>2b1e4f6a-0000-4000-8000-000000000001</x1TransactionId>)" +
           inner_body + R"(
  </x1RequestMessage>
</X1Request>)";
}

const char* kActivate = R"(
    <taskDetails>
      <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>
      <targetIdentifiers>
        <targetIdentifier><supiimsi>262019999999999</supiimsi></targetIdentifier>
      </targetIdentifiers>
      <deliveryType>X2andX3</deliveryType>
      <listOfDIDs><dId>11111111-1111-4111-8111-111111111111</dId></listOfDIDs>
    </taskDetails>)";

TEST(LiX1, ParsesActivateTaskAndReadsTheTargetIdentifier) {
    auto r = parse_request(envelope("ActivateTaskRequest", kActivate));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->requests.size(), 1U);
    const auto& req = r->requests[0];
    EXPECT_EQ(req.type, MessageType::ActivateTask);
    EXPECT_EQ(req.header.admf_identifier, "admf-1");
    EXPECT_EQ(req.header.x1_transaction_id, "2b1e4f6a-0000-4000-8000-000000000001");
    const auto& t = std::get<ActivateTask>(req.body).task;
    EXPECT_EQ(t.xid, "3fa85f64-5717-4562-b3fc-2c963f66afa6");
    EXPECT_EQ(t.delivery, DeliveryType::X2AndX3);
    ASSERT_EQ(t.targets.size(), 1U);
    EXPECT_EQ(t.targets[0].kind, TargetIdentifierKind::SupiImsi);
    EXPECT_EQ(t.targets[0].element, "supiimsi");
    EXPECT_EQ(t.targets[0].value, "262019999999999");
    ASSERT_EQ(t.dids.size(), 1U);
    EXPECT_EQ(t.dids[0], "11111111-1111-4111-8111-111111111111");
}

TEST(LiX1, RejectsANonSchemaValidDocumentAsTopLevelError) {
    // deliveryType carries a value the enum forbids -> schema-invalid -> TopLevelError.
    const std::string bad = envelope("ActivateTaskRequest", R"(
    <taskDetails>
      <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>
      <targetIdentifiers><targetIdentifier><imsi>262019999999999</imsi></targetIdentifier></targetIdentifiers>
      <deliveryType>X9Only</deliveryType>
      <listOfDIDs><dId>11111111-1111-4111-8111-111111111111</dId></listOfDIDs>
    </taskDetails>)");
    auto r = parse_request(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(r.error().top_level);
    // The best-effort header still carries the identifiers for the TopLevelError response.
    ASSERT_TRUE(r.error().header.has_value());
    EXPECT_EQ(r.error().header->admf_identifier, "admf-1");
}

TEST(LiX1, RejectsExternalEntityXxe) {
    // X1 is served to a network peer (the ADMF). A DOCTYPE declaring an external-file entity used
    // inside a target identifier must NOT resolve the file into the tree (XML_PARSE_NOENT is off).
    // libxml2 leaves the reference unexpanded, so the identifier fails the schema pattern and the
    // request is rejected as a TopLevelError -- and no file content is ever substituted.
    const std::string xxe =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE X1Request [ <!ENTITY xxe SYSTEM "file:///etc/hostname"> ]>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <x1RequestMessage xsi:type="ActivateTaskRequest">
    <admfIdentifier>admf-1</admfIdentifier>
    <neIdentifier>amf-1</neIdentifier>
    <messageTimestamp>2026-09-16T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>2b1e4f6a-0000-4000-8000-000000000001</x1TransactionId>
    <taskDetails>
      <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>
      <targetIdentifiers><targetIdentifier><supiimsi>&xxe;</supiimsi></targetIdentifier></targetIdentifiers>
      <deliveryType>X2andX3</deliveryType>
      <listOfDIDs><dId>11111111-1111-4111-8111-111111111111</dId></listOfDIDs>
    </taskDetails>
  </x1RequestMessage>
</X1Request>)";
    auto r = parse_request(xxe);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(r.error().top_level);
    // If any parse path did surface a value, it must never carry substituted file content.
    if (r.has_value() && !r->requests.empty()) {
        const auto& t = std::get<ActivateTask>(r->requests[0].body).task;
        ASSERT_FALSE(t.targets.empty());
        EXPECT_EQ(t.targets[0].value.find('/'), std::string::npos);
    }
}

TEST(LiX1, ServerAcceptsActivateAndAnswersAValidResponse) {
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    std::string stored_xid;
    cb.activate_task = [&](const TaskDetails& t) -> std::optional<ErrorCode> {
        stored_xid = t.xid;
        return std::nullopt;
    };
    const std::string resp = handle_request(envelope("ActivateTaskRequest", kActivate), cb);
    EXPECT_EQ(stored_xid, "3fa85f64-5717-4562-b3fc-2c963f66afa6");
    EXPECT_NE(resp.find("ActivateTaskResponse"), std::string::npos) << resp;
    EXPECT_NE(resp.find("AcknowledgedAndCompleted"), std::string::npos) << resp;
    // The response is itself a valid X1Response (serialise_response validated it before returning).
    EXPECT_NE(resp.find("X1Response"), std::string::npos);
}

TEST(LiX1, DuplicateXidIsErrorCode2010) {
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    cb.activate_task = [&](const TaskDetails&) -> std::optional<ErrorCode> {
        return ErrorCode::XidAlreadyExists;
    };
    const std::string resp = handle_request(envelope("ActivateTaskRequest", kActivate), cb);
    EXPECT_NE(resp.find("ErrorResponse"), std::string::npos) << resp;
    EXPECT_NE(resp.find("<errorCode>2010</errorCode>"), std::string::npos) << resp;
    EXPECT_NE(resp.find("ActivateTask"), std::string::npos); // requestMessageType echoed
}

TEST(LiX1, UnsupportedRequestTypeIsError1080) {
    // GetTaskDetails parses but is answered 1080 this increment.
    const std::string body = R"(
    <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>)";
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    const std::string resp = handle_request(envelope("GetTaskDetailsRequest", body), cb);
    EXPECT_NE(resp.find("<errorCode>1080</errorCode>"), std::string::npos) << resp;
}

TEST(LiX1, PingIsAcknowledged) {
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    const std::string resp = handle_request(envelope("PingRequest", ""), cb);
    EXPECT_NE(resp.find("PingResponse"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("ErrorResponse"), std::string::npos);
}

TEST(LiX1, DeactivateAndCreateDestinationRoundTrip) {
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    std::string removed;
    cb.deactivate_task = [&](const std::string& xid) -> std::optional<ErrorCode> {
        removed = xid;
        return std::nullopt;
    };
    std::string created_did;
    cb.create_destination = [&](const DestinationDetails& d) -> std::optional<ErrorCode> {
        created_did = d.did;
        return std::nullopt;
    };
    const std::string deact = handle_request(
        envelope("DeactivateTaskRequest", "\n    <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>"),
        cb);
    EXPECT_EQ(removed, "3fa85f64-5717-4562-b3fc-2c963f66afa6");
    EXPECT_NE(deact.find("DeactivateTaskResponse"), std::string::npos) << deact;

    const std::string dest_body = R"(
    <destinationDetails>
      <dId>22222222-2222-4222-8222-222222222222</dId>
      <deliveryType>X2andX3</deliveryType>
      <deliveryAddress><uri>https://mdf2.example.net/x2</uri></deliveryAddress>
    </destinationDetails>)";
    const std::string cre = handle_request(envelope("CreateDestinationRequest", dest_body), cb);
    EXPECT_EQ(created_did, "22222222-2222-4222-8222-222222222222");
    EXPECT_NE(cre.find("CreateDestinationResponse"), std::string::npos) << cre;
}

TEST(LiX1, IdentityCheckRejectsBeforeStoreAction) {
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    bool store_called = false;
    cb.activate_task = [&](const TaskDetails&) -> std::optional<ErrorCode> {
        store_called = true;
        return std::nullopt;
    };
    cb.check_identity = [&](const MessageHeader& h) -> std::optional<ErrorCode> {
        return h.admf_identifier == "admf-1"
                   ? std::optional<ErrorCode>(ErrorCode::UnexpectedAdmfIdentifier)
                   : std::nullopt;
    };
    const std::string resp = handle_request(envelope("ActivateTaskRequest", kActivate), cb);
    EXPECT_FALSE(store_called);
    EXPECT_NE(resp.find("<errorCode>1040</errorCode>"), std::string::npos) << resp;
}

// ---- keepalive machine (6.6.2) ----

TEST(LiX1Keepalive, RaisesFaultAfterP2AndClearsOnNextRequest) {
    KeepaliveMonitor::Config cfg;
    cfg.time_p2 = 100s;
    cfg.time_p1 = 10s;
    cfg.allow_deactivate_all = false;
    KeepaliveMonitor km(cfg);
    auto t0 = std::chrono::steady_clock::time_point(0s);
    EXPECT_EQ(km.on_x1_request(t0), KeepaliveMonitor::Action::None);
    EXPECT_EQ(km.tick(t0 + 50s), KeepaliveMonitor::Action::None);
    EXPECT_EQ(km.tick(t0 + 101s), KeepaliveMonitor::Action::SendFaultReport);
    // A request while the fault stands clears it.
    EXPECT_EQ(km.on_x1_request(t0 + 105s), KeepaliveMonitor::Action::SendFaultCleared);
    EXPECT_EQ(km.tick(t0 + 106s), KeepaliveMonitor::Action::None);
}

TEST(LiX1Keepalive, DeactivatesAllTasksWhenAllowedAndAdmfNeverAcks) {
    KeepaliveMonitor::Config cfg;
    cfg.time_p2 = 100s;
    cfg.time_p1 = 10s;
    cfg.allow_deactivate_all = true;
    KeepaliveMonitor km(cfg);
    auto t0 = std::chrono::steady_clock::time_point(0s);
    km.on_x1_request(t0);
    EXPECT_EQ(km.tick(t0 + 101s), KeepaliveMonitor::Action::SendFaultReport);
    // P1 expires with no ack -> deactivate all.
    EXPECT_EQ(km.tick(t0 + 112s), KeepaliveMonitor::Action::DeactivateAllTasks);
}

TEST(LiX1Keepalive, AckStartsP3ThenDeactivatesIfStillSilent) {
    KeepaliveMonitor::Config cfg;
    cfg.time_p2 = 100s;
    cfg.time_p1 = 10s;
    cfg.time_p3 = 200s;
    cfg.allow_deactivate_all = true;
    KeepaliveMonitor km(cfg);
    auto t0 = std::chrono::steady_clock::time_point(0s);
    km.on_x1_request(t0);
    EXPECT_EQ(km.tick(t0 + 101s), KeepaliveMonitor::Action::SendFaultReport);
    EXPECT_EQ(km.on_fault_report_ack(t0 + 105s), KeepaliveMonitor::Action::None);
    EXPECT_EQ(km.tick(t0 + 200s), KeepaliveMonitor::Action::None); // P3 not up yet
    EXPECT_EQ(km.tick(t0 + 306s), KeepaliveMonitor::Action::DeactivateAllTasks);
}

} // namespace
