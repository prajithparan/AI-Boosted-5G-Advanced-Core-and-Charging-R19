// ETSI TS 103 120 LI_HI1 XML codec (ADR-0462 step 2). The strongest oracle available is ETSI's own
// example messages (specs/etsi/103120/examples/xml, vendored with the schemas): every one of them
// must parse, round-trip through the writer, and the writer's output must itself validate (the
// codec refuses to emit anything that does not). The typed views are asserted against the values
// printed in those examples; object edits are checked for schema ORDER by validating the result.

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "li_core/hi1.hpp"

#include <gtest/gtest.h>

namespace {

using namespace li_core::hi1;
namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path examples() {
    return fs::path(HI1_EXAMPLES_DIR);
}

Header sample_header() {
    Header h;
    h.sender = {"XX", "ACTOR01"};
    h.receiver = {"XX", "ACTOR02"};
    h.transaction_id = "c02358b2-76cf-4ba4-a8eb-f6436ccaea2e";
    h.timestamp = "2026-10-06T12:00:00.000000Z";
    h.version = {"V1.23.1", "XX", "v1.0"};
    return h;
}

const std::set<std::string> kNeedsForeignSchema = {"request5-XML-Delivery.xml"};

TEST(LiHi1, EveryEtsiExampleRequestAndResponseParses) {
    int requests = 0;
    int responses = 0;
    for (const auto& entry : fs::directory_iterator(examples())) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".xml" || name == "FooServiceSchema.xsd") {
            continue;
        }
        const std::string xml = slurp(entry.path());
        if (xml.find("<HI1Message") == std::string::npos) {
            continue; // example.pem etc.
        }
        if (name.rfind("request", 0) == 0) {
            const auto r = parse_request(xml);
            if (kNeedsForeignSchema.count(name) != 0) {
                EXPECT_FALSE(r.has_value()) << name << ": its extension content needs ETSI's example-only Foo schema";
                continue;
            }
            EXPECT_TRUE(r.has_value()) << name << ": " << (r ? "" : r.error().detail);
            ++requests;
        } else if (name.rfind("response", 0) == 0) {
            const auto r = parse_response(xml);
            EXPECT_TRUE(r.has_value()) << name << ": " << (r ? "" : r.error().detail);
            ++responses;
        }
    }
    EXPECT_GE(requests, 15);
    EXPECT_GE(responses, 8);
}

TEST(LiHi1, TheLiLifecycleExampleReadsBackTheValuesEtsiPrinted) {
    const auto r = parse_request(slurp(examples() / "request1.xml"));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ(r->header.sender.unique_identifier, "ACTOR01");
    EXPECT_EQ(r->header.receiver.unique_identifier, "ACTOR02");
    EXPECT_EQ(r->header.transaction_id, "c02358b2-76cf-4ba4-a8eb-f6436ccaea2e");
    EXPECT_EQ(r->header.version.etsi_version, "V1.13.1");
    ASSERT_GE(r->actions.size(), 2U);

    const auto* auth_create = std::get_if<CreateAction>(&r->actions[0].body);
    ASSERT_NE(auth_create, nullptr);
    const Object& auth = auth_create->object;
    EXPECT_EQ(auth.type(), ObjectType::Authorisation);
    EXPECT_EQ(auth.type_name(), "AuthorisationObject");
    EXPECT_EQ(auth.identifier(), "7dbbc880-8750-4d3c-abe7-ea4a17646045");
    const auto av = view_authorisation(auth);
    EXPECT_EQ(av.reference, "W000001");
    EXPECT_EQ(av.start_time, "2015-09-01T12:00:00Z");
    EXPECT_EQ(av.end_time, "2015-12-01T12:00:00Z");
    EXPECT_FALSE(av.status.has_value());

    const auto* task_create = std::get_if<CreateAction>(&r->actions[1].body);
    ASSERT_NE(task_create, nullptr);
    const Object& task = task_create->object;
    EXPECT_EQ(task.type(), ObjectType::LITask);
    const auto tv = view_litask(task);
    EXPECT_EQ(tv.liid, "LIID1");
    ASSERT_EQ(tv.targets.size(), 1U);
    EXPECT_EQ(tv.targets[0], (TargetValue{"ETSI", "InternationalE164", "442079460223"}));
    ASSERT_TRUE(tv.service_type.has_value());
    EXPECT_EQ(tv.service_type->value, "DataType1");
    ASSERT_TRUE(tv.delivery_type.has_value());
    EXPECT_EQ(tv.delivery_type->value, "IRIandCC");
    ASSERT_EQ(tv.destinations.size(), 1U);
    EXPECT_EQ(tv.destinations[0].address_kind, "IPv4Address");
    EXPECT_EQ(tv.destinations[0].address, "192.0.2.0");
    ASSERT_EQ(tv.associated.size(), 1U);
    EXPECT_EQ(tv.associated[0], "7dbbc880-8750-4d3c-abe7-ea4a17646045"); // belongs to the Authorisation
}

TEST(LiHi1, EtsiExamplesRoundTripThroughTheWriter) {
    for (const char* name : {"request1.xml", "request2.xml", "request3.xml", "request6.xml", "request8_LPTaskObject.xml"}) {
        const auto first = parse_request(slurp(examples() / name));
        ASSERT_TRUE(first.has_value()) << name << ": " << first.error().detail;
        const auto xml = serialise_request(*first); // validated against the schema inside
        ASSERT_TRUE(xml.has_value()) << name << ": " << xml.error();
        const auto second = parse_request(*xml);
        ASSERT_TRUE(second.has_value()) << name << ": " << second.error().detail;
        EXPECT_EQ(second->header, first->header) << name;
        ASSERT_EQ(second->actions.size(), first->actions.size()) << name;
        for (std::size_t i = 0; i < first->actions.size(); ++i) {
            EXPECT_EQ(second->actions[i].id, first->actions[i].id) << name;
            EXPECT_EQ(second->actions[i].body.index(), first->actions[i].body.index()) << name;
            const auto* a = std::get_if<CreateAction>(&first->actions[i].body);
            const auto* b = std::get_if<CreateAction>(&second->actions[i].body);
            if (a != nullptr && b != nullptr) {
                EXPECT_EQ(b->object.identifier(), a->object.identifier()) << name;
                EXPECT_EQ(b->object.type_name(), a->object.type_name()) << name;
            }
        }
    }
}

TEST(LiHi1, EtsiResponsesRoundTripIncludingTheCspConfig) {
    for (const char* name : {"response1.xml", "response2.xml", "response3.xml", "response_config.xml"}) {
        const auto first = parse_response(slurp(examples() / name));
        ASSERT_TRUE(first.has_value()) << name << ": " << first.error().detail;
        const auto xml = serialise_response(*first);
        ASSERT_TRUE(xml.has_value()) << name << ": " << xml.error();
        const auto second = parse_response(*xml);
        ASSERT_TRUE(second.has_value()) << name << ": " << second.error().detail;
        EXPECT_EQ(second->header, first->header) << name;
        EXPECT_EQ(second->payload.index(), first->payload.index()) << name;
    }
    const auto cfg = parse_response(slurp(examples() / "response_config.xml"));
    ASSERT_TRUE(cfg.has_value());
    const auto& results = std::get<std::vector<ActionResult>>(cfg->payload);
    ASSERT_EQ(results.size(), 1U);
    const auto* config = std::get_if<ConfigResult>(&results[0].outcome);
    ASSERT_NE(config, nullptr);
    ASSERT_EQ(config->config.targeting.size(), 2U);
    EXPECT_EQ(config->config.targeting[0].format_name, "InternationalE164");
    ASSERT_EQ(config->config.li_endpoints.size(), 1U);
    EXPECT_EQ(config->config.li_endpoints[0].endpoint.value, "NewAuthorisation");
    EXPECT_EQ(config->config.li_endpoints[0].url, "https://ts103120.example.com/li/authorisation/new");
    ASSERT_TRUE(config->config.target_formats.has_value());
    EXPECT_EQ(config->config.target_formats->entries[0].format_name, "ProprietaryIdentifier");
}

Object parsed_authorisation() {
    const auto r = parse_request(slurp(examples() / "request1.xml"));
    return std::get<CreateAction>(r->actions[0].body).object;
}

TEST(LiHi1, ObjectEditsLandAtTheirSchemaPositionAndTheMessageStillValidates) {
    Object auth = parsed_authorisation();
    // AuthorisationStatus belongs between Reference and Timespan; DesiredStatus right after it;
    // Generation is a base member that precedes every derived one.
    ASSERT_TRUE(auth.set_entry("AuthorisationDesiredStatus", {"ETSI", "AuthorisationDesiredStatus", "Approved"}).has_value());
    ASSERT_TRUE(auth.set_entry("AuthorisationStatus", {"ETSI", "AuthorisationStatus", "AwaitingApproval"}).has_value());
    ASSERT_TRUE(auth.set_text("Generation", "3").has_value());
    ASSERT_TRUE(auth.set_text("LastChanged", "2026-10-06T12:00:00Z").has_value());
    ASSERT_TRUE(auth.set_text("AuthorisationServedTimestamp", "2026-10-06T12:00:01Z").has_value());

    Response resp;
    resp.header = sample_header();
    resp.payload = std::vector<ActionResult>{{0, GetResult{auth}}};
    const auto xml = serialise_response(resp); // schema validation inside: wrong order would fail
    ASSERT_TRUE(xml.has_value()) << xml.error();

    // Replace in place (no duplicate), and read back.
    ASSERT_TRUE(auth.set_entry("AuthorisationStatus", {"ETSI", "AuthorisationStatus", "Approved"}).has_value());
    const auto view = view_authorisation(auth);
    ASSERT_TRUE(view.status.has_value());
    EXPECT_EQ(view.status->value, "Approved");
    EXPECT_EQ(view.desired_status->value, "Approved");
    EXPECT_EQ(auth.text("Generation"), "3");
    EXPECT_EQ(view.reference, "W000001"); // untouched members survive
    resp.payload = std::vector<ActionResult>{{0, GetResult{auth}}};
    EXPECT_TRUE(serialise_response(resp).has_value());

    // A member the type does not have is an error, never a guess; remove works.
    EXPECT_FALSE(auth.set_text("Reference", "x").has_value()); // that is an LITask member
    EXPECT_FALSE(auth.set_text("NoSuchField", "x").has_value());
    EXPECT_TRUE(auth.remove("AuthorisationServedTimestamp"));
    EXPECT_FALSE(auth.remove("AuthorisationServedTimestamp"));
}

TEST(LiHi1, TextValuesAreEscapedNotInterpreted) {
    Object auth = parsed_authorisation();
    ASSERT_TRUE(auth.set_text("AuthorisationManualInformation", "a < b && c > \"d\" </x>").has_value());
    EXPECT_EQ(auth.text("AuthorisationManualInformation"), "a < b && c > \"d\" </x>");
}

TEST(LiHi1, ResponsesOfEveryKindValidateAndParseBack) {
    Object auth = parsed_authorisation();
    Response resp;
    resp.header = sample_header();
    ListRecord rec;
    rec.object_type = {"ETSI", "ObjectType", "Authorisation"};
    rec.identifier = auth.identifier();
    rec.country_code = "XX";
    rec.generation = 2;
    rec.last_changed = "2026-10-06T12:00:00Z";
    resp.payload = std::vector<ActionResult>{
        {0, CreateResult{auth.identifier(), auth}},
        {1, UpdateResult{auth.identifier(), std::nullopt}},
        {2, GetResult{auth}},
        {3, ListResult{{rec}}},
        {4, DeliverResult{auth.identifier()}},
        {5, Failure{static_cast<std::uint32_t>(ErrorCode::UpdateObjectDoesNotExist), "no such object"}},
    };
    const auto xml = serialise_response(resp);
    ASSERT_TRUE(xml.has_value()) << xml.error();
    const auto back = parse_response(*xml);
    ASSERT_TRUE(back.has_value()) << back.error().detail;
    const auto& results = std::get<std::vector<ActionResult>>(back->payload);
    ASSERT_EQ(results.size(), 6U);
    EXPECT_TRUE(std::holds_alternative<CreateResult>(results[0].outcome));
    EXPECT_FALSE(std::get<UpdateResult>(results[1].outcome).object.has_value());
    EXPECT_EQ(std::get<ListResult>(results[3].outcome).records[0].generation, 2U);
    const auto& failure = std::get<Failure>(results[5].outcome);
    EXPECT_EQ(failure.code, 3011U);
    EXPECT_EQ(failure.description, "no such object");

    // A message-level failure (9.2.2) is a top-level ErrorInformation, no actions.
    resp.payload = Failure{static_cast<std::uint32_t>(ErrorCode::ValidationError), "bad message"};
    const auto top = serialise_response(resp);
    ASSERT_TRUE(top.has_value()) << top.error();
    const auto top_back = parse_response(*top);
    ASSERT_TRUE(top_back.has_value());
    EXPECT_EQ(std::get<Failure>(top_back->payload).code, 3020U);
}

TEST(LiHi1, ARequestOfEveryVerbValidatesAndParsesBack) {
    Object auth = parsed_authorisation();
    Request req;
    req.header = sample_header();
    ListAction list;
    list.object_type = DictionaryEntry{"ETSI", "ObjectType", "LITask"};
    list.maximum_object_count = 10;
    list.status = DictionaryEntry{"ETSI", "TaskStatus", "Active"};
    list.country_code = "XX";
    req.actions = {{0, GetAction{auth.identifier()}},
                   {1, CreateAction{auth}},
                   {2, UpdateAction{auth}},
                   {3, list},
                   {4, DeliverAction{auth.identifier(), auth}},
                   {5, GetCspConfigAction{}}};
    const auto xml = serialise_request(req);
    ASSERT_TRUE(xml.has_value()) << xml.error();
    const auto back = parse_request(*xml);
    ASSERT_TRUE(back.has_value()) << back.error().detail;
    ASSERT_EQ(back->actions.size(), 6U);
    EXPECT_EQ(std::get<GetAction>(back->actions[0].body).identifier, auth.identifier());
    const auto& l = std::get<ListAction>(back->actions[3].body);
    EXPECT_EQ(l.maximum_object_count, 10U);
    EXPECT_EQ(l.status->value, "Active");
    EXPECT_TRUE(std::holds_alternative<GetCspConfigAction>(back->actions[5].body));
}

TEST(LiHi1, ANotificationObjectIsBuiltAndValidates) {
    NotificationParams p;
    p.identifier = "9f1d1d0e-2a6c-4b6b-8f0e-7c1d2e3f4a5b";
    p.country_code = "XX";
    p.owner_identifier = "ACTOR02";
    p.details = "Authorisation W000001 approved & actioned";
    p.type = {"ETSI", "NotificationType", "General"};
    p.timestamp = "2026-10-06T12:00:00Z";
    p.associated = {"7dbbc880-8750-4d3c-abe7-ea4a17646045"};
    p.statuses = {{"7dbbc880-8750-4d3c-abe7-ea4a17646045",
                   {"ETSI", "AuthorisationStatus", "Approved"},
                   std::string("done")}};
    const auto n = make_notification(p);
    ASSERT_TRUE(n.has_value()) << n.error();
    EXPECT_EQ(n->type(), ObjectType::Notification);
    EXPECT_EQ(n->text("NotificationDetails"), "Authorisation W000001 approved & actioned");
    Response resp;
    resp.header = sample_header();
    resp.payload = std::vector<ActionResult>{{0, GetResult{*n}}};
    const auto xml = serialise_response(resp);
    ASSERT_TRUE(xml.has_value()) << xml.error();
}

TEST(LiHi1, BadMessagesAreRejectedWithTheDetailAndTheHeader) {
    const auto garbage = parse_request("this is not xml");
    ASSERT_FALSE(garbage.has_value());
    EXPECT_EQ(garbage.error().code, ErrorCode::ValidationError);
    EXPECT_FALSE(garbage.error().header.has_value());

    EXPECT_FALSE(parse_request("<Other/>").has_value());

    // Schema-invalid: delete the mandatory ObjectIdentifier of the first object.
    std::string xml = slurp(examples() / "request1.xml");
    const auto start = xml.find("<ObjectIdentifier>");
    const auto end = xml.find("</ObjectIdentifier>", start) + std::string("</ObjectIdentifier>").size();
    xml.erase(start, end - start);
    const auto invalid = parse_request(xml);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code, ErrorCode::ValidationError);
    EXPECT_FALSE(invalid.error().detail.empty());
    ASSERT_TRUE(invalid.error().header.has_value()); // the sender can still be answered
    EXPECT_EQ(invalid.error().header->sender.unique_identifier, "ACTOR01");

    // A response handed to the request parser (and vice versa) is refused, not misread.
    EXPECT_FALSE(parse_request(slurp(examples() / "response1.xml")).has_value());
    EXPECT_FALSE(parse_response(slurp(examples() / "request1.xml")).has_value());

    // XXE: an external entity must not be expanded into a value.
    const std::string xxe = R"(<?xml version="1.0"?>
<!DOCTYPE m [<!ENTITY x SYSTEM "file:///etc/passwd">]>
<HI1Message xmlns="http://uri.etsi.org/03120/common/2019/10/Core"><Header>&x;</Header></HI1Message>)";
    EXPECT_FALSE(parse_request(xxe).has_value());
}

TEST(LiHi1, TheWriterNeverEmitsAnInvalidMessage) {
    Request req;
    req.header = sample_header();
    req.header.transaction_id = "not-a-uuid"; // etsi:UUID pattern
    req.actions = {{0, GetCspConfigAction{}}};
    EXPECT_FALSE(serialise_request(req).has_value());
    req.header = sample_header();
    req.actions.clear();
    EXPECT_FALSE(serialise_request(req).has_value()); // at least one action
}

TEST(LiHi1, EtsiVersionPattern) {
    EXPECT_TRUE(is_valid_etsi_version("V1.23.1"));
    EXPECT_TRUE(is_valid_etsi_version("V10.2.33"));
    EXPECT_FALSE(is_valid_etsi_version("1.23.1"));
    EXPECT_FALSE(is_valid_etsi_version("V1.23"));
    EXPECT_FALSE(is_valid_etsi_version("V1.23.1.4"));
    EXPECT_FALSE(is_valid_etsi_version("V1..1"));
    EXPECT_FALSE(is_valid_etsi_version(""));
}

} // namespace
