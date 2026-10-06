// ETSI TS 103 221-1 V1.23.1 LI_X1 ADMF-side codec (ADR-0462): the requests an ADMF's LIPF builds,
// the responses it reads, and the NE -> ADMF Report requests. Every built request is checked by
// parsing it back through the NE-side parser (which validates against the committed XSD), so the
// two halves of the codec cannot drift apart; responses are produced by the NE-side serialiser
// and read back, plus hand-written documents for the shapes the NE side does not emit.

#include <string>

#include "li_core/x1.hpp"
#include "li_core/x1_server.hpp"

#include <gtest/gtest.h>

namespace {

using namespace li_core::x1;

MessageHeader header(const std::string& txn = "2b1e4f6a-0000-4000-8000-0000000000a1") {
    MessageHeader h;
    h.admf_identifier = "admf-1";
    h.ne_identifier = "amf-1";
    h.message_timestamp = "2026-10-06T00:00:00.000000Z";
    h.version = "v1.23.1";
    h.x1_transaction_id = txn;
    return h;
}

Request make(MessageType type, RequestBody body, const std::string& txn = "2b1e4f6a-0000-4000-8000-0000000000a1") {
    Request r;
    r.header = header(txn);
    r.type = type;
    r.body = std::move(body);
    return r;
}

TaskDetails full_task() {
    TaskDetails t;
    t.xid = "3fa85f64-5717-4562-b3fc-2c963f66afa6";
    t.targets.push_back({TargetIdentifierKind::SupiImsi, "supiimsi", "999700000000001"});
    t.targets.push_back({TargetIdentifierKind::SupiNai, "supinai", "user@operator.example"});
    t.delivery = DeliveryType::X2Only;
    t.dids = {"11111111-1111-4111-8111-111111111111"};
    MediationDetails md;
    md.liid = "LIID-2026-0001";
    md.delivery = MediationDeliveryType::Hi2Only;
    md.dids = {"22222222-2222-4222-8222-222222222222"};
    t.mediation_details.push_back(md);
    t.correlation_id = 42;
    t.implicit_deactivation_allowed = false;
    t.identifier_association_events = IdentifierAssociationEventsGenerated::All;
    return t;
}

TEST(LiX1Client, ActivateTaskRoundTripsEveryModelledMember) {
    const auto xml = serialise_request({make(MessageType::ActivateTask, ActivateTask{full_task()})});
    ASSERT_TRUE(xml.has_value()) << xml.error();

    const auto parsed = parse_request(*xml);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail;
    ASSERT_EQ(parsed->requests.size(), 1U);
    EXPECT_EQ(parsed->requests[0].header.x1_transaction_id, "2b1e4f6a-0000-4000-8000-0000000000a1");
    const auto& t = std::get<ActivateTask>(parsed->requests[0].body).task;
    const auto want = full_task();
    EXPECT_EQ(t.xid, want.xid);
    ASSERT_EQ(t.targets.size(), 2U);
    EXPECT_EQ(t.targets[0].kind, TargetIdentifierKind::SupiImsi);
    EXPECT_EQ(t.targets[0].value, "999700000000001");
    EXPECT_EQ(t.targets[1].kind, TargetIdentifierKind::SupiNai);
    EXPECT_EQ(t.delivery, DeliveryType::X2Only);
    EXPECT_EQ(t.dids, want.dids);
    ASSERT_EQ(t.mediation_details.size(), 1U);
    EXPECT_EQ(t.mediation_details[0].liid, "LIID-2026-0001");
    EXPECT_EQ(t.mediation_details[0].delivery, MediationDeliveryType::Hi2Only);
    EXPECT_EQ(t.mediation_details[0].dids, want.mediation_details[0].dids);
    EXPECT_EQ(t.correlation_id, 42U);
    ASSERT_TRUE(t.implicit_deactivation_allowed.has_value());
    EXPECT_FALSE(*t.implicit_deactivation_allowed);
    ASSERT_TRUE(t.identifier_association_events.has_value());
    EXPECT_EQ(*t.identifier_association_events, IdentifierAssociationEventsGenerated::All);
}

TEST(LiX1Client, GatingAbsentAndIdentifierAssociationOnlyRoundTrip) {
    TaskDetails t = full_task();
    t.identifier_association_events.reset();
    auto xml = serialise_request({make(MessageType::ModifyTask, ModifyTask{t})});
    ASSERT_TRUE(xml.has_value()) << xml.error();
    auto parsed = parse_request(*xml);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail;
    EXPECT_EQ(parsed->requests[0].type, MessageType::ModifyTask);
    EXPECT_FALSE(std::get<ModifyTask>(parsed->requests[0].body).task.identifier_association_events);

    t.identifier_association_events = IdentifierAssociationEventsGenerated::IdentifierAssociation;
    xml = serialise_request({make(MessageType::ModifyTask, ModifyTask{t})});
    ASSERT_TRUE(xml.has_value()) << xml.error();
    parsed = parse_request(*xml);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*std::get<ModifyTask>(parsed->requests[0].body).task.identifier_association_events,
              IdentifierAssociationEventsGenerated::IdentifierAssociation);
}

TEST(LiX1Client, EveryOtherBuildableRequestTypeIsSchemaValidAndParsesBack) {
    DestinationDetails v4;
    v4.did = "11111111-1111-4111-8111-111111111111";
    v4.friendly_name = "lemf-a";
    v4.delivery = DeliveryType::X2AndX3;
    v4.address = {DeliveryAddress::Kind::IpAddressAndPort, "192.0.2.10:8443"};
    DestinationDetails v6 = v4;
    v6.did = "22222222-2222-4222-8222-222222222222";
    v6.friendly_name.reset();
    v6.address = {DeliveryAddress::Kind::IpAddressAndPort, "2001:db8::1:8443"};
    DestinationDetails uri = v4;
    uri.did = "33333333-3333-4333-8333-333333333333";
    uri.address = {DeliveryAddress::Kind::Uri, "https://lemf.example/deliver"};

    const std::vector<Request> all{
        make(MessageType::DeactivateTask,
             DeactivateTask{"3fa85f64-5717-4562-b3fc-2c963f66afa6"}),
        make(MessageType::DeactivateAllTasks, DeactivateAllTasks{}),
        make(MessageType::GetTaskDetails, GetTaskDetails{"3fa85f64-5717-4562-b3fc-2c963f66afa6"}),
        make(MessageType::CreateDestination, CreateDestination{v4}),
        make(MessageType::CreateDestination, CreateDestination{v6}),
        make(MessageType::CreateDestination, CreateDestination{uri}),
        make(MessageType::RemoveDestination, RemoveDestination{v4.did}),
        make(MessageType::RemoveAllDestinations, RemoveAllDestinations{}),
        make(MessageType::Ping, Ping{}),
        make(MessageType::Keepalive, Keepalive{}),
    };
    // Several requests in one X1Request (6.1: "one or more").
    const auto xml = serialise_request(all);
    ASSERT_TRUE(xml.has_value()) << xml.error();
    const auto parsed = parse_request(*xml);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail;
    ASSERT_EQ(parsed->requests.size(), all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(parsed->requests[i].type, all[i].type) << i;
    }
    const auto& d4 = std::get<CreateDestination>(parsed->requests[3].body).destination;
    EXPECT_EQ(d4.did, v4.did);
    EXPECT_EQ(d4.friendly_name, "lemf-a");
    EXPECT_EQ(d4.address.kind, DeliveryAddress::Kind::IpAddressAndPort);
    EXPECT_EQ(d4.address.value, "192.0.2.10:8443");
    const auto& d6 = std::get<CreateDestination>(parsed->requests[4].body).destination;
    // TS 103 280 IPv6Address is the fixed 8x4-hex form, so the writer expands "2001:db8::1".
    EXPECT_EQ(d6.address.value, "2001:0db8:0000:0000:0000:0000:0000:0001:8443");
    const auto& du = std::get<CreateDestination>(parsed->requests[5].body).destination;
    EXPECT_EQ(du.address.kind, DeliveryAddress::Kind::Uri);
    EXPECT_EQ(du.address.value, "https://lemf.example/deliver");
}

TEST(LiX1Client, AnInvalidRequestIsNeverEmitted) {
    // 7.2.1: only schema-valid messages are sent. A non-UUID xId violates the XId type.
    TaskDetails t = full_task();
    t.xid = "not-a-uuid";
    EXPECT_FALSE(serialise_request({make(MessageType::ActivateTask, ActivateTask{t})}).has_value());
    // No requests at all, and NE -> ADMF messages, are not buildable by an ADMF.
    EXPECT_FALSE(serialise_request({}).has_value());
    EXPECT_FALSE(
        serialise_request({make(MessageType::ReportNEIssue, ReportNEIssue{})}).has_value());
}

TEST(LiX1Client, ParsesOkAndErrorResponsesTheNeSideProduces) {
    const MessageHeader h = header();
    const auto xml = serialise_response({
        OkResponse{h, MessageType::ActivateTask, true},
        OkResponse{h, MessageType::ModifyTask, false},
        ErrorResponse{h, MessageType::ActivateTask, ErrorCode::XidAlreadyExists, "task exists"},
        OkResponse{h, MessageType::Keepalive, true},
    });
    ASSERT_TRUE(xml.has_value()) << xml.error();

    const auto parsed = parse_response(*xml);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail;
    ASSERT_EQ(parsed->size(), 4U);
    const auto* ok = std::get_if<OkResponse>(&(*parsed)[0]);
    ASSERT_NE(ok, nullptr);
    EXPECT_EQ(ok->type, MessageType::ActivateTask);
    EXPECT_TRUE(ok->acknowledged_and_completed);
    EXPECT_EQ(ok->header.x1_transaction_id, h.x1_transaction_id);
    const auto* acked = std::get_if<OkResponse>(&(*parsed)[1]);
    ASSERT_NE(acked, nullptr);
    EXPECT_EQ(acked->type, MessageType::ModifyTask);
    EXPECT_FALSE(acked->acknowledged_and_completed); // "Acknowledged": still in progress
    const auto* err = std::get_if<ErrorResponse>(&(*parsed)[2]);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->type, MessageType::ActivateTask);
    EXPECT_EQ(err->code, ErrorCode::XidAlreadyExists);
    EXPECT_EQ(err->description, "task exists");
    const auto* ka = std::get_if<OkResponse>(&(*parsed)[3]);
    ASSERT_NE(ka, nullptr);
    EXPECT_EQ(ka->type, MessageType::Keepalive);
}

TEST(LiX1Client, ParsesAGetTaskDetailsResponseWithStatusAndFaults) {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Response xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <x1ResponseMessage xsi:type="GetTaskDetailsResponse">
    <admfIdentifier>admf-1</admfIdentifier>
    <neIdentifier>amf-1</neIdentifier>
    <messageTimestamp>2026-10-06T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>2b1e4f6a-0000-4000-8000-0000000000a2</x1TransactionId>
    <taskResponseDetails>
      <taskDetails>
        <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>
        <targetIdentifiers><targetIdentifier><supiimsi>999700000000001</supiimsi></targetIdentifier></targetIdentifiers>
        <deliveryType>X2Only</deliveryType>
        <listOfDIDs><dId>11111111-1111-4111-8111-111111111111</dId></listOfDIDs>
      </taskDetails>
      <taskStatus>
        <provisioningStatus>failed</provisioningStatus>
        <listOfFaults>
          <unresolvedFault><errorCode>3010</errorCode><errorDescription>target not supported</errorDescription></unresolvedFault>
        </listOfFaults>
      </taskStatus>
    </taskResponseDetails>
  </x1ResponseMessage>
</X1Response>)";
    const auto parsed = parse_response(xml);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail;
    ASSERT_EQ(parsed->size(), 1U);
    const auto* r = std::get_if<TaskDetailsResponse>(&(*parsed)[0]);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->task.xid, "3fa85f64-5717-4562-b3fc-2c963f66afa6");
    ASSERT_EQ(r->task.targets.size(), 1U);
    EXPECT_EQ(r->task.targets[0].value, "999700000000001");
    EXPECT_EQ(r->provisioning, ProvisioningStatus::Failed);
    ASSERT_EQ(r->unresolved_faults.size(), 1U);
    EXPECT_EQ(r->unresolved_faults[0].code, 3010);
    EXPECT_EQ(r->unresolved_faults[0].description, "target not supported");
}

TEST(LiX1Client, TopLevelErrorAndGarbageAreErrorsNotResponses) {
    const auto tle = parse_response(serialise_top_level_error(header()));
    ASSERT_FALSE(tle.has_value());
    EXPECT_TRUE(tle.error().top_level);
    ASSERT_TRUE(tle.error().header.has_value());
    EXPECT_EQ(tle.error().header->ne_identifier, "amf-1");

    EXPECT_FALSE(parse_response("this is not xml").has_value());
    EXPECT_FALSE(parse_response("<X1Response xmlns=\"http://uri.etsi.org/03221/X1/2017/10\"/>")
                     .has_value()); // schema: at least one x1ResponseMessage
    // An XXE attempt must not be expanded (same hardening as parse_request).
    const auto xxe = parse_response(R"(<?xml version="1.0"?>
<!DOCTYPE r [<!ENTITY x SYSTEM "file:///etc/passwd">]>
<X1Response xmlns="http://uri.etsi.org/03221/X1/2017/10"><x1ResponseMessage>&x;</x1ResponseMessage></X1Response>)");
    EXPECT_FALSE(xxe.has_value());
}

std::string report_envelope(const std::string& type, const std::string& body) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <x1RequestMessage xsi:type=")" +
           type + R"(">
    <admfIdentifier>admf-1</admfIdentifier>
    <neIdentifier>amf-1</neIdentifier>
    <messageTimestamp>2026-10-06T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>2b1e4f6a-0000-4000-8000-0000000000a3</x1TransactionId>)" +
           body + R"(
  </x1RequestMessage>
</X1Request>)";
}

TEST(LiX1Client, ParsesTheNeToAdmfReportRequests) {
    auto ne = parse_request(report_envelope("ReportNEIssueRequest", R"(
    <typeOfNeIssueMessage>FaultReport</typeOfNeIssueMessage>
    <description>no X1 request within TIME_P2</description>
    <issueCode>1234</issueCode>)"));
    ASSERT_TRUE(ne.has_value()) << ne.error().detail;
    ASSERT_EQ(ne->requests.size(), 1U);
    EXPECT_EQ(ne->requests[0].type, MessageType::ReportNEIssue);
    const auto& n = std::get<ReportNEIssue>(ne->requests[0].body);
    EXPECT_EQ(n.type, NeIssueType::FaultReport);
    EXPECT_EQ(n.description, "no X1 request within TIME_P2");
    EXPECT_EQ(n.issue_code, 1234);

    auto task = parse_request(report_envelope("ReportTaskIssueRequest", R"(
    <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>
    <taskReportType>TerminatingFault</taskReportType>
    <taskIssueErrorCode>3010</taskIssueErrorCode>
    <taskIssueDetails>unsupported identifier</taskIssueDetails>)"));
    ASSERT_TRUE(task.has_value()) << task.error().detail;
    EXPECT_EQ(task->requests[0].type, MessageType::ReportTaskIssue);
    const auto& t = std::get<ReportTaskIssue>(task->requests[0].body);
    EXPECT_EQ(t.xid, "3fa85f64-5717-4562-b3fc-2c963f66afa6");
    EXPECT_EQ(t.report_type, TaskReportType::TerminatingFault);
    EXPECT_EQ(t.error_code, 3010);
    EXPECT_EQ(t.details, "unsupported identifier");

    // The ADMF's own OK answer to a Report* is a schema-valid ReportNEIssueResponse /
    // ReportTaskIssueResponse, built by the same serialiser.
    const auto answer = serialise_response({OkResponse{header(), MessageType::ReportNEIssue, true},
                                            OkResponse{header(), MessageType::ReportTaskIssue, true}});
    ASSERT_TRUE(answer.has_value()) << answer.error();
    const auto back = parse_response(*answer);
    ASSERT_TRUE(back.has_value()) << back.error().detail;
    EXPECT_EQ(std::get<OkResponse>((*back)[0]).type, MessageType::ReportNEIssue);
    EXPECT_EQ(std::get<OkResponse>((*back)[1]).type, MessageType::ReportTaskIssue);

    // An NE (this library's server side) that is sent a Report* answers 1080, not silence.
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    const std::string reply = handle_request(
        report_envelope("ReportNEIssueRequest", R"(
    <typeOfNeIssueMessage>Warning</typeOfNeIssueMessage><description>x</description>)"),
        cb);
    EXPECT_NE(reply.find("1080"), std::string::npos) << reply;
}

TEST(LiX1Client, AnAdmfServerHandlesReportsThroughItsCallbacksAndAnNeAnswers1080) {
    int task_reports = 0;
    int ne_reports = 0;
    TaskStoreCallbacks admf;
    admf.ne_identifier = "admf-1";
    admf.report_task_issue = [&](const ReportTaskIssue& r) -> std::optional<ErrorCode> {
        ++task_reports;
        return r.xid == "3fa85f64-5717-4562-b3fc-2c963f66afa6" ? std::nullopt : std::optional(ErrorCode::XidDoesNotExist);
    };
    admf.report_ne_issue = [&](const ReportNEIssue&) -> std::optional<ErrorCode> {
        ++ne_reports;
        return std::nullopt;
    };
    const std::string known = report_envelope("ReportTaskIssueRequest", R"(
    <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId><taskReportType>TerminatingFault</taskReportType>)");
    const std::string unknown = report_envelope("ReportTaskIssueRequest", R"(
    <xId>11111111-1111-4111-8111-111111111111</xId><taskReportType>Warning</taskReportType>)");
    EXPECT_EQ(handle_request(known, admf).find("ErrorResponse"), std::string::npos);
    EXPECT_NE(handle_request(unknown, admf).find("2020"), std::string::npos); // refused with the callback's code
    const std::string ne_issue = report_envelope("ReportNEIssueRequest", R"(
    <typeOfNeIssueMessage>FaultReport</typeOfNeIssueMessage><description>x</description>)");
    EXPECT_NE(handle_request(ne_issue, admf).find("ReportNEIssueResponse"), std::string::npos);
    EXPECT_EQ(task_reports, 2);
    EXPECT_EQ(ne_reports, 1);

    TaskStoreCallbacks ne; // an NE installs neither
    ne.ne_identifier = "amf-1";
    EXPECT_NE(handle_request(known, ne).find("1080"), std::string::npos);
}

} // namespace
