// ADR-0440: the per-task IdentifierAssociationExtensions gating parameter the IRI-POI in the AMF
// reads from an LI_X1 ActivateTask/ModifyTask (TS 33.128 V19.7.0 table 6.2.2.1.1-1/-2, clause
// 6.2.2.2.1). Element names and enumeration values are the 3GPP X1 extension schema's
// (specs/3gpp/33128-attachments/urn_3GPP_ns_li_3GPPX1Extensions.xsd, namespace
// urn:3GPP:ns:li:3GPPX1Extensions:r19:v4); the container is ETSI TS 103 221-1 V1.23.1's
// TaskDetails.taskDetailsExtensions (an Extension: Owner + ##other strict wildcard). Every document
// here goes through li_core::x1::parse_request, i.e. is schema-validated first -- so these tests
// also prove the combined ETSI + 3GPP schema set loads in libxml2.

#include <string>

#include "li_core/x1.hpp"
#include "li_core/x1_server.hpp"

#include <gtest/gtest.h>

namespace {

using namespace li_core::x1;

constexpr const char* kTgppNs = "urn:3GPP:ns:li:3GPPX1Extensions:r19:v4";

std::string envelope(const std::string& type, const std::string& extensions) {
    return std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:tgpp=")") +
           kTgppNs + R"(">
  <x1RequestMessage xsi:type=")" +
           type + R"(">
    <admfIdentifier>admf-1</admfIdentifier>
    <neIdentifier>amf-1</neIdentifier>
    <messageTimestamp>2026-09-25T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>2b1e4f6a-0000-4000-8000-000000000001</x1TransactionId>
    <taskDetails>
      <xId>3fa85f64-5717-4562-b3fc-2c963f66afa6</xId>
      <targetIdentifiers>
        <targetIdentifier><supiimsi>262019999999999</supiimsi></targetIdentifier>
      </targetIdentifiers>
      <deliveryType>X2Only</deliveryType>
      <listOfDIDs><dId>11111111-1111-4111-8111-111111111111</dId></listOfDIDs>)" +
           extensions + R"(
    </taskDetails>
  </x1RequestMessage>
</X1Request>)";
}

// TS 33.128 table 6.2.2.1.1-1's path, literally:
// TaskDetailsExtensions/IdentifierAssociationExtensions.
std::string direct_form(const std::string& value) {
    return R"(
      <taskDetailsExtensions>
        <Owner>3GPP</Owner>
        <tgpp:IdentifierAssociationExtensions>
          <tgpp:IdentifierAssociationEventsGenerated>)" +
           value + R"(</tgpp:IdentifierAssociationEventsGenerated>
        </tgpp:IdentifierAssociationExtensions>
      </taskDetailsExtensions>)";
}

// The same type reached through the schema's X1Extensions/X1Extension choice.
std::string wrapped_form(const std::string& value) {
    return R"(
      <taskDetailsExtensions>
        <Owner>3GPP</Owner>
        <tgpp:X1Extensions>
          <tgpp:IdentifierAssociation>
            <tgpp:IdentifierAssociationEventsGenerated>)" +
           value + R"(</tgpp:IdentifierAssociationEventsGenerated>
          </tgpp:IdentifierAssociation>
        </tgpp:X1Extensions>
      </taskDetailsExtensions>)";
}

TaskDetails parse_task(const std::string& xml) {
    auto r = parse_request(xml);
    EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().detail);
    if (!r.has_value() || r->requests.size() != 1U) {
        return {};
    }
    const auto& body = r->requests[0].body;
    if (const auto* a = std::get_if<ActivateTask>(&body)) {
        return a->task;
    }
    if (const auto* m = std::get_if<ModifyTask>(&body)) {
        return m->task;
    }
    ADD_FAILURE() << "not an ActivateTask/ModifyTask";
    return {};
}

} // namespace

// Canary: a task with no extension still validates against the widened schema set, and carries
// no gating -- table 6.2.2.1.1-1: absent means Identifier(De)Association records are not generated.
TEST(LiX1Gating, AbsentExtensionMeansNoGating) {
    const auto t = parse_task(envelope("ActivateTaskRequest", ""));
    EXPECT_EQ(t.xid, "3fa85f64-5717-4562-b3fc-2c963f66afa6");
    EXPECT_FALSE(t.identifier_association_events.has_value());
}

TEST(LiX1Gating, DirectFormIdentifierAssociation) {
    const auto t =
        parse_task(envelope("ActivateTaskRequest", direct_form("IdentifierAssociation")));
    ASSERT_TRUE(t.identifier_association_events.has_value());
    EXPECT_EQ(*t.identifier_association_events,
              IdentifierAssociationEventsGenerated::IdentifierAssociation);
}

TEST(LiX1Gating, DirectFormAll) {
    const auto t = parse_task(envelope("ActivateTaskRequest", direct_form("All")));
    ASSERT_TRUE(t.identifier_association_events.has_value());
    EXPECT_EQ(*t.identifier_association_events, IdentifierAssociationEventsGenerated::All);
}

TEST(LiX1Gating, X1ExtensionsChoiceFormBothValues) {
    const auto a =
        parse_task(envelope("ActivateTaskRequest", wrapped_form("IdentifierAssociation")));
    ASSERT_TRUE(a.identifier_association_events.has_value());
    EXPECT_EQ(*a.identifier_association_events,
              IdentifierAssociationEventsGenerated::IdentifierAssociation);
    const auto b = parse_task(envelope("ActivateTaskRequest", wrapped_form("All")));
    ASSERT_TRUE(b.identifier_association_events.has_value());
    EXPECT_EQ(*b.identifier_association_events, IdentifierAssociationEventsGenerated::All);
}

// The XSD enumeration is closed ({IdentifierAssociation, All}); anything else fails schema
// validation, which TS 103 221-1 clause 6.1 answers with a TopLevelError.
TEST(LiX1Gating, ValueOutsideTheEnumerationIsATopLevelError) {
    const auto r = parse_request(envelope("ActivateTaskRequest", direct_form("Everything")));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(r.error().top_level);
}

// The mandatory sub-element missing is likewise a schema failure, not a silent "absent".
TEST(LiX1Gating, MissingEventsGeneratedIsATopLevelError) {
    const auto r = parse_request(envelope("ActivateTaskRequest", R"(
      <taskDetailsExtensions>
        <Owner>3GPP</Owner>
        <tgpp:IdentifierAssociationExtensions/>
      </taskDetailsExtensions>)"));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(r.error().top_level);
}

// An unrelated 3GPP extension (the SMSF's, same schema) is valid and does NOT enable gating.
TEST(LiX1Gating, UnrelatedThreeGppExtensionDoesNotGate) {
    const auto t = parse_task(envelope("ActivateTaskRequest", R"(
      <taskDetailsExtensions>
        <Owner>3GPP</Owner>
        <tgpp:X1Extensions><tgpp:SMSFExtensions/></tgpp:X1Extensions>
      </taskDetailsExtensions>)"));
    EXPECT_FALSE(t.identifier_association_events.has_value());
}

// An element that merely shares the local name but lives in another namespace is not the 3GPP
// parameter. The schema set has no declaration for that namespace, so the strict wildcard rejects
// the whole document -- it can never be misread as gating.
TEST(LiX1Gating, SameLocalNameInAForeignNamespaceIsRejected) {
    const auto r = parse_request(envelope("ActivateTaskRequest", R"(
      <taskDetailsExtensions>
        <Owner>vendor</Owner>
        <v:IdentifierAssociationExtensions xmlns:v="urn:example:vendor">
          <v:IdentifierAssociationEventsGenerated>All</v:IdentifierAssociationEventsGenerated>
        </v:IdentifierAssociationExtensions>
      </taskDetailsExtensions>)"));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(r.error().top_level);
}

// Several taskDetailsExtensions (maxOccurs unbounded): the gating one is found wherever it sits.
TEST(LiX1Gating, FoundAmongSeveralExtensions) {
    const auto t = parse_task(envelope("ActivateTaskRequest", R"(
      <taskDetailsExtensions>
        <Owner>3GPP</Owner>
        <tgpp:X1Extensions><tgpp:SMSFExtensions/></tgpp:X1Extensions>
      </taskDetailsExtensions>)" + direct_form("All")));
    ASSERT_TRUE(t.identifier_association_events.has_value());
    EXPECT_EQ(*t.identifier_association_events, IdentifierAssociationEventsGenerated::All);
}

// ModifyTask carries a full TaskDetails, so it can set -- and, by omitting the extension, clear --
// the gating.
TEST(LiX1Gating, ModifyTaskCarriesTheGating) {
    const auto set =
        parse_task(envelope("ModifyTaskRequest", direct_form("IdentifierAssociation")));
    ASSERT_TRUE(set.identifier_association_events.has_value());
    const auto cleared = parse_task(envelope("ModifyTaskRequest", ""));
    EXPECT_FALSE(cleared.identifier_association_events.has_value());
}

// The NE server hands the parsed gating to the store callback unchanged.
TEST(LiX1Gating, ServerPassesGatingToTheActivateCallback) {
    std::optional<IdentifierAssociationEventsGenerated> seen;
    bool called = false;
    TaskStoreCallbacks cb;
    cb.ne_identifier = "amf-1";
    cb.activate_task = [&](const TaskDetails& d) {
        called = true;
        seen = d.identifier_association_events;
        return std::optional<ErrorCode>{};
    };
    const auto resp = handle_request(envelope("ActivateTaskRequest", wrapped_form("All")), cb);
    EXPECT_TRUE(called) << resp;
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(*seen, IdentifierAssociationEventsGenerated::All);
    EXPECT_NE(resp.find("ActivateTaskResponse"), std::string::npos) << resp;
}
