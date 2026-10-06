#include "li_core/x1.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlschemas.h>
#include <mutex>
#include <string_view>
#include <utility>

// ETSI TS 103 221-1 V1.23.1 X1 codec over libxml2 (ADR-0372). libxml2 stays inside li_core's
// shared object -- it is linked PRIVATE and no li_core:: header names an libxml2 type.

namespace li_core::x1 {

namespace {

constexpr const char* kX1Ns = "http://uri.etsi.org/03221/X1/2017/10";
constexpr const char* kXsiNs = "http://www.w3.org/2001/XMLSchema-instance";
// The 3GPP X1 extension namespace (TS 33.128 V19.7.0 attachment
// urn_3GPP_ns_li_3GPPX1Extensions.xsd). Extension content is matched on namespace AND local name:
// an ETSI Extension is <xs:any namespace="##other"/>, so a bare local-name match could pick up
// an unrelated vendor's element of the same name.
constexpr const char* k3gppX1ExtNs = "urn:3GPP:ns:li:3GPPX1Extensions:r19:v4";

// The one schema, loaded once (the wrapper that wires the 103280 + HashedID imports). The X1
// interface is served on NF worker threads (the AMF POI, increment 4), so libxml2's global
// tables must be initialised before any concurrent parse -- xmlInitParser() here, under the same
// std::call_once that builds the schema. xmlSchemaValidateDoc writes into the schema's compiled
// type cache, so it is NOT safe to share a schema across concurrent validations; schema_valid()
// serialises them (see kSchemaMutex).
xmlSchemaPtr load_schema() {
    static std::once_flag once;
    static xmlSchemaPtr schema = nullptr;
    std::call_once(once, [] {
        xmlInitParser();
        const std::string path = std::string(LI_ETSI_SCHEMA_DIR) + "/103221-1/x1-validation.xsd";
        if (xmlSchemaParserCtxtPtr pc = xmlSchemaNewParserCtxt(path.c_str())) {
            schema = xmlSchemaParse(pc);
            xmlSchemaFreeParserCtxt(pc);
        }
    });
    return schema;
}

bool schema_valid(xmlDocPtr doc) {
    xmlSchemaPtr schema = load_schema();
    if (schema == nullptr) {
        return false; // no schema on disk -> cannot claim validity
    }
    // xmlSchemaValidateDoc mutates the shared schema's compiled-type cache, so concurrent
    // validations against the one schema race; serialise them. Validation is microseconds on
    // X1-sized documents, so a single lock is cheaper than a schema per thread.
    static std::mutex kSchemaMutex;
    std::lock_guard<std::mutex> lock(kSchemaMutex);
    xmlSchemaValidCtxtPtr vc = xmlSchemaNewValidCtxt(schema);
    const int rc = xmlSchemaValidateDoc(vc, doc);
    xmlSchemaFreeValidCtxt(vc);
    return rc == 0;
}

std::string node_text(xmlNodePtr node) {
    xmlChar* c = xmlNodeGetContent(node);
    std::string out = c != nullptr ? reinterpret_cast<const char*>(c) : "";
    xmlFree(c);
    return out;
}

bool is(xmlNodePtr node, const char* name) {
    return node->type == XML_ELEMENT_NODE &&
           xmlStrcmp(node->name, reinterpret_cast<const xmlChar*>(name)) == 0;
}

xmlNodePtr child(xmlNodePtr parent, const char* name) {
    for (xmlNodePtr n = parent->children; n != nullptr; n = n->next) {
        if (is(n, name)) {
            return n;
        }
    }
    return nullptr;
}

std::optional<std::string> child_text(xmlNodePtr parent, const char* name) {
    if (xmlNodePtr n = child(parent, name)) {
        return node_text(n);
    }
    return std::nullopt;
}

// The xsi:type local name (strips any "x1:" prefix), e.g. "ActivateTaskRequest".
std::string xsi_type(xmlNodePtr node) {
    xmlChar* t = xmlGetNsProp(
        node, reinterpret_cast<const xmlChar*>("type"), reinterpret_cast<const xmlChar*>(kXsiNs));
    std::string v = t != nullptr ? reinterpret_cast<const char*>(t) : "";
    xmlFree(t);
    if (const auto colon = v.find(':'); colon != std::string::npos) {
        v = v.substr(colon + 1);
    }
    return v;
}

MessageHeader read_header(xmlNodePtr msg) {
    MessageHeader h;
    h.admf_identifier = child_text(msg, "admfIdentifier").value_or("");
    h.ne_identifier = child_text(msg, "neIdentifier").value_or("");
    h.message_timestamp = child_text(msg, "messageTimestamp").value_or("");
    h.version = child_text(msg, "version").value_or("");
    h.x1_transaction_id = child_text(msg, "x1TransactionId").value_or("");
    return h;
}

struct KindMap {
    const char* element;
    TargetIdentifierKind kind;
};
constexpr std::array<KindMap, 13> kTargetKinds{{
    {"supiimsi", TargetIdentifierKind::SupiImsi},
    {"supinai", TargetIdentifierKind::SupiNai},
    {"suci", TargetIdentifierKind::Suci},
    {"peiImei", TargetIdentifierKind::PeiImei},
    {"peiImeisv", TargetIdentifierKind::PeiImeisv},
    {"gpsiMsisdn", TargetIdentifierKind::GpsiMsisdn},
    {"gpsiNai", TargetIdentifierKind::GpsiNai},
    {"imsi", TargetIdentifierKind::Imsi},
    {"imei", TargetIdentifierKind::Imei},
    {"e164Number", TargetIdentifierKind::Msisdn},
    {"ipv4Address", TargetIdentifierKind::Ipv4Address},
    {"ipv6Address", TargetIdentifierKind::Ipv6Address},
    {"nai", TargetIdentifierKind::Nai},
}};

TargetIdentifier read_target(xmlNodePtr ti) {
    TargetIdentifier out;
    for (xmlNodePtr n = ti->children; n != nullptr; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) {
            continue;
        }
        out.element = reinterpret_cast<const char*>(n->name);
        out.value = node_text(n);
        out.kind = TargetIdentifierKind::Other;
        for (const auto& k : kTargetKinds) {
            if (out.element == k.element) {
                out.kind = k.kind;
                break;
            }
        }
        break; // the schema's <xs:choice> permits exactly one
    }
    return out;
}

bool is_ns(xmlNodePtr node, const char* ns, const char* name) {
    return is(node, name) && node->ns != nullptr && node->ns->href != nullptr &&
           xmlStrcmp(node->ns->href, reinterpret_cast<const xmlChar*>(ns)) == 0;
}

xmlNodePtr child_ns(xmlNodePtr parent, const char* ns, const char* name) {
    for (xmlNodePtr n = parent->children; n != nullptr; n = n->next) {
        if (is_ns(n, ns, name)) {
            return n;
        }
    }
    return nullptr;
}

// An IdentifierAssociationExtensions-typed element (either the global
// <IdentifierAssociationExtensions> or <X1Extensions><IdentifierAssociation>): its one mandatory
// child IdentifierAssociationEventsGenerated, an xs:string enumeration {IdentifierAssociation,
// All}. The document is schema-validated before this runs, so any other value never gets here;
// nullopt is defensive only.
std::optional<IdentifierAssociationEventsGenerated> read_identifier_association(xmlNodePtr ext) {
    xmlNodePtr eg = child_ns(ext, k3gppX1ExtNs, "IdentifierAssociationEventsGenerated");
    if (eg == nullptr) {
        return std::nullopt;
    }
    const std::string v = node_text(eg);
    if (v == "IdentifierAssociation") {
        return IdentifierAssociationEventsGenerated::IdentifierAssociation;
    }
    if (v == "All") {
        return IdentifierAssociationEventsGenerated::All;
    }
    return std::nullopt;
}

// TS 33.128 table 6.2.2.1.1-1: TaskDetailsExtensions/IdentifierAssociationExtensions. A
// TaskDetails may carry several taskDetailsExtensions (maxOccurs unbounded), each an ETSI
// Extension (Owner + ##other wildcard content); scan them all.
std::optional<IdentifierAssociationEventsGenerated> read_task_gating(xmlNodePtr td) {
    for (xmlNodePtr ext = td->children; ext != nullptr; ext = ext->next) {
        if (!is(ext, "taskDetailsExtensions")) {
            continue;
        }
        for (xmlNodePtr n = ext->children; n != nullptr; n = n->next) {
            if (is_ns(n, k3gppX1ExtNs, "IdentifierAssociationExtensions")) {
                if (auto g = read_identifier_association(n)) {
                    return g;
                }
            } else if (is_ns(n, k3gppX1ExtNs, "X1Extensions")) {
                if (xmlNodePtr ia = child_ns(n, k3gppX1ExtNs, "IdentifierAssociation")) {
                    if (auto g = read_identifier_association(ia)) {
                        return g;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

DeliveryType read_delivery(const std::string& s) {
    if (s == "X2Only") {
        return DeliveryType::X2Only;
    }
    if (s == "X3Only") {
        return DeliveryType::X3Only;
    }
    return DeliveryType::X2AndX3;
}

TaskDetails read_task_details(xmlNodePtr td) {
    TaskDetails task;
    task.xid = child_text(td, "xId").value_or("");
    if (xmlNodePtr list = child(td, "targetIdentifiers")) {
        for (xmlNodePtr n = list->children; n != nullptr; n = n->next) {
            if (is(n, "targetIdentifier")) {
                task.targets.push_back(read_target(n));
            }
        }
    }
    task.delivery = read_delivery(child_text(td, "deliveryType").value_or(""));
    if (xmlNodePtr list = child(td, "listOfDIDs")) {
        for (xmlNodePtr n = list->children; n != nullptr; n = n->next) {
            if (is(n, "dId")) {
                task.dids.push_back(node_text(n));
            } else if (is(n, "dSId")) {
                task.dsids.push_back(node_text(n));
            }
        }
    }
    task.product_id = child_text(td, "productID");
    // Annex C.2.2: listOfMediationDetails is where an MDF learns the LIID(s) of the task.
    if (xmlNodePtr list = child(td, "listOfMediationDetails")) {
        for (xmlNodePtr n = list->children; n != nullptr; n = n->next) {
            if (!is(n, "mediationDetails")) {
                continue;
            }
            MediationDetails details;
            details.liid = child_text(n, "LIID").value_or("");
            const std::string delivery = child_text(n, "deliveryType").value_or("");
            details.delivery = delivery == "HI2Only"   ? MediationDeliveryType::Hi2Only
                               : delivery == "HI3Only" ? MediationDeliveryType::Hi3Only
                                                       : MediationDeliveryType::Hi2AndHi3;
            details.start_time = child_text(n, "StartTime");
            details.end_time = child_text(n, "EndTime");
            if (xmlNodePtr dids = child(n, "listOfDIDs")) {
                for (xmlNodePtr d = dids->children; d != nullptr; d = d->next) {
                    if (is(d, "dId")) {
                        details.dids.push_back(node_text(d));
                    }
                }
            }
            task.mediation_details.push_back(std::move(details));
        }
    }
    if (auto c = child_text(td, "correlationID")) {
        task.correlation_id = std::strtoull(c->c_str(), nullptr, 10);
    }
    if (auto c = child_text(td, "implicitDeactivationAllowed")) {
        task.implicit_deactivation_allowed = (*c == "true" || *c == "1");
    }
    task.identifier_association_events = read_task_gating(td);
    return task;
}

DestinationDetails read_destination(xmlNodePtr dd) {
    DestinationDetails dest;
    dest.did = child_text(dd, "dId").value_or("");
    dest.friendly_name = child_text(dd, "friendlyName");
    dest.delivery = read_delivery(child_text(dd, "deliveryType").value_or(""));
    if (xmlNodePtr addr = child(dd, "deliveryAddress")) {
        if (xmlNodePtr ip = child(addr, "ipAddressAndPort")) {
            dest.address.kind = DeliveryAddress::Kind::IpAddressAndPort;
            // TS 103 280 IPAddressPort ::= address + port. Both children, and their own children,
            // are element names declared by the TS 103 280 schema -- which is
            // elementFormDefault="qualified", so an instance document carries them in that
            // namespace. `child()` matches on local name, which is what makes this work whatever
            // prefix the ADMF chose. The names are the schema's exactly: IPv4Address and
            // IPv6Address capitalise the "IP", and port wraps a TCPPort or a UDPPort rather than
            // holding the number itself.
            std::string a;
            if (xmlNodePtr addrn = child(ip, "address")) {
                if (xmlNodePtr v4 = child(addrn, "IPv4Address")) {
                    a = node_text(v4);
                } else if (xmlNodePtr v6 = child(addrn, "IPv6Address")) {
                    a = node_text(v6);
                }
            }
            std::string port;
            if (xmlNodePtr portn = child(ip, "port")) {
                if (xmlNodePtr tcp = child(portn, "TCPPort")) {
                    port = node_text(tcp);
                } else if (xmlNodePtr udp = child(portn, "UDPPort")) {
                    port = node_text(udp);
                }
            }
            dest.address.value = a + ":" + port;
        } else if (auto e164 = child_text(addr, "e164Number")) {
            dest.address.kind = DeliveryAddress::Kind::E164Number;
            dest.address.value = *e164;
        } else if (auto uri = child_text(addr, "uri")) {
            dest.address.kind = DeliveryAddress::Kind::Uri;
            dest.address.value = *uri;
        } else if (auto em = child_text(addr, "emailAddress")) {
            dest.address.kind = DeliveryAddress::Kind::EmailAddress;
            dest.address.value = *em;
        }
    }
    return dest;
}

struct TypeMap {
    const char* xsi;
    MessageType type;
};
constexpr std::array<TypeMap, 12> kTypes{{
    {"ActivateTaskRequest", MessageType::ActivateTask},
    {"ModifyTaskRequest", MessageType::ModifyTask},
    {"DeactivateTaskRequest", MessageType::DeactivateTask},
    {"DeactivateAllTasksRequest", MessageType::DeactivateAllTasks},
    {"GetTaskDetailsRequest", MessageType::GetTaskDetails},
    {"CreateDestinationRequest", MessageType::CreateDestination},
    {"RemoveDestinationRequest", MessageType::RemoveDestination},
    {"RemoveAllDestinationsRequest", MessageType::RemoveAllDestinations},
    {"PingRequest", MessageType::Ping},
    {"KeepaliveRequest", MessageType::Keepalive},
    {"ReportTaskIssueRequest", MessageType::ReportTaskIssue},
    {"ReportNEIssueRequest", MessageType::ReportNEIssue},
}};

TaskReportType read_task_report_type(const std::string& v) {
    if (v == "AllClear") {
        return TaskReportType::AllClear;
    }
    if (v == "NonTerminatingFault") {
        return TaskReportType::NonTerminatingFault;
    }
    if (v == "TerminatingFault") {
        return TaskReportType::TerminatingFault;
    }
    if (v == "ImplicitDeactivation") {
        return TaskReportType::ImplicitDeactivation;
    }
    if (v == "FullyActionedAndSuccessful") {
        return TaskReportType::FullyActionedAndSuccessful;
    }
    if (v == "FullyActionedAndUnsuccessful") {
        return TaskReportType::FullyActionedAndUnsuccessful;
    }
    return TaskReportType::Warning;
}

NeIssueType read_ne_issue_type(const std::string& v) {
    if (v == "FaultCleared") {
        return NeIssueType::FaultCleared;
    }
    if (v == "FaultReport") {
        return NeIssueType::FaultReport;
    }
    if (v == "Alert") {
        return NeIssueType::Alert;
    }
    return NeIssueType::Warning;
}

Request read_request(xmlNodePtr msg) {
    Request req;
    req.header = read_header(msg);
    const std::string xt = xsi_type(msg);
    req.type = MessageType::Unsupported;
    for (const auto& t : kTypes) {
        if (xt == t.xsi) {
            req.type = t.type;
            break;
        }
    }
    switch (req.type) {
        case MessageType::ActivateTask:
            req.body = ActivateTask{child(msg, "taskDetails") != nullptr
                                        ? read_task_details(child(msg, "taskDetails"))
                                        : TaskDetails{}};
            break;
        case MessageType::ModifyTask:
            req.body = ModifyTask{child(msg, "taskDetails") != nullptr
                                      ? read_task_details(child(msg, "taskDetails"))
                                      : TaskDetails{}};
            break;
        case MessageType::DeactivateTask:
            req.body = DeactivateTask{child_text(msg, "xId").value_or("")};
            break;
        case MessageType::DeactivateAllTasks:
            req.body = DeactivateAllTasks{};
            break;
        case MessageType::GetTaskDetails:
            req.body = GetTaskDetails{child_text(msg, "xId").value_or("")};
            break;
        case MessageType::CreateDestination:
            req.body = CreateDestination{child(msg, "destinationDetails") != nullptr
                                             ? read_destination(child(msg, "destinationDetails"))
                                             : DestinationDetails{}};
            break;
        case MessageType::RemoveDestination:
            req.body = RemoveDestination{child_text(msg, "dId").value_or("")};
            break;
        case MessageType::RemoveAllDestinations:
            req.body = RemoveAllDestinations{};
            break;
        case MessageType::Ping:
            req.body = Ping{};
            break;
        case MessageType::Keepalive:
            req.body = Keepalive{};
            break;
        case MessageType::ReportTaskIssue: {
            ReportTaskIssue r;
            r.xid = child_text(msg, "xId").value_or("");
            r.report_type = read_task_report_type(child_text(msg, "taskReportType").value_or(""));
            if (auto c = child_text(msg, "taskIssueErrorCode")) {
                r.error_code = std::atoi(c->c_str());
            }
            r.details = child_text(msg, "taskIssueDetails");
            req.body = std::move(r);
            break;
        }
        case MessageType::ReportNEIssue: {
            ReportNEIssue r;
            r.type = read_ne_issue_type(child_text(msg, "typeOfNeIssueMessage").value_or(""));
            r.description = child_text(msg, "description").value_or("");
            if (auto c = child_text(msg, "issueCode")) {
                r.issue_code = std::atoi(c->c_str());
            }
            req.body = std::move(r);
            break;
        }
        case MessageType::Unsupported:
            req.body = Unsupported{xt};
            break;
    }
    return req;
}

// ---- building ----

void set_ns_type(xmlNodePtr node, xmlNsPtr xsi, const char* type) {
    xmlSetNsProp(node,
                 xsi,
                 reinterpret_cast<const xmlChar*>("type"),
                 reinterpret_cast<const xmlChar*>((std::string("x1:") + type).c_str()));
}

void add_text(xmlNodePtr parent, const char* name, const std::string& value) {
    xmlNewTextChild(parent,
                    parent->ns,
                    reinterpret_cast<const xmlChar*>(name),
                    reinterpret_cast<const xmlChar*>(value.c_str()));
}

void write_header(xmlNodePtr msg, const MessageHeader& h) {
    add_text(msg, "admfIdentifier", h.admf_identifier);
    add_text(msg, "neIdentifier", h.ne_identifier);
    add_text(msg, "messageTimestamp", h.message_timestamp);
    add_text(msg, "version", h.version);
    add_text(msg, "x1TransactionId", h.x1_transaction_id);
}

const char* response_xsi_type(MessageType t) {
    switch (t) {
        case MessageType::ActivateTask:
            return "ActivateTaskResponse";
        case MessageType::ModifyTask:
            return "ModifyTaskResponse";
        case MessageType::DeactivateTask:
            return "DeactivateTaskResponse";
        case MessageType::DeactivateAllTasks:
            return "DeactivateAllTasksResponse";
        case MessageType::GetTaskDetails:
            return "GetTaskDetailsResponse";
        case MessageType::CreateDestination:
            return "CreateDestinationResponse";
        case MessageType::RemoveDestination:
            return "RemoveDestinationResponse";
        case MessageType::RemoveAllDestinations:
            return "RemoveAllDestinationsResponse";
        case MessageType::Ping:
            return "PingResponse";
        case MessageType::Keepalive:
            return "KeepaliveResponse";
        case MessageType::ReportTaskIssue:
            return "ReportTaskIssueResponse";
        case MessageType::ReportNEIssue:
            return "ReportNEIssueResponse";
        case MessageType::Unsupported:
            return "ErrorResponse";
    }
    return "ErrorResponse";
}

} // namespace

const char* message_type_name(MessageType t) {
    switch (t) {
        case MessageType::ActivateTask:
            return "ActivateTask";
        case MessageType::ModifyTask:
            return "ModifyTask";
        case MessageType::DeactivateTask:
            return "DeactivateTask";
        case MessageType::DeactivateAllTasks:
            return "DeactivateAllTasks";
        case MessageType::GetTaskDetails:
            return "GetTaskDetails";
        case MessageType::CreateDestination:
            return "CreateDestination";
        case MessageType::RemoveDestination:
            return "RemoveDestination";
        case MessageType::RemoveAllDestinations:
            return "RemoveAllDestinations";
        case MessageType::Ping:
            return "Ping";
        case MessageType::Keepalive:
            return "Keepalive";
        case MessageType::ReportTaskIssue:
            return "ReportTaskIssue";
        case MessageType::ReportNEIssue:
            return "ReportNEIssue";
        case MessageType::Unsupported:
            return "ExtendedRequestMessageType";
    }
    return "ExtendedRequestMessageType";
}

const char* schema_path() {
    static const std::string p = std::string(LI_ETSI_SCHEMA_DIR) + "/103221-1/x1-validation.xsd";
    return p.c_str();
}

tl::expected<TargetIdentifier, std::string> parse_target_identifier_fragment(std::string_view xml) {
    // The attribute carries the CHILDREN of a TargetIdentifier element, so it has no single root:
    // wrap it in one. Same hardened settings as parse_request -- NONET on, NOENT deliberately off
    // (this fragment reaches us from an X2 PDU, which is as attacker-facing as X1).
    const std::string wrapped = "<TargetIdentifier>" + std::string(xml) + "</TargetIdentifier>";
    xmlDocPtr doc = xmlReadMemory(
        wrapped.data(), static_cast<int>(wrapped.size()), "ti.xml", nullptr, XML_PARSE_NONET);
    if (doc == nullptr) {
        return tl::make_unexpected(
            std::string("target identifier fragment is not well-formed XML"));
    }
    struct DocGuard {
        xmlDocPtr d;
        ~DocGuard() { xmlFreeDoc(d); }
    } guard{doc};

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root == nullptr) {
        return tl::make_unexpected(std::string("target identifier fragment is empty"));
    }
    TargetIdentifier out = read_target(root);
    if (out.element.empty()) {
        return tl::make_unexpected(std::string("target identifier fragment has no element"));
    }
    return out;
}

tl::expected<RequestContainer, ParseError> parse_request(const std::string& xml) {
    // XML_PARSE_NONET blocks network fetches; NOENT is deliberately NOT set. With NOENT libxml2
    // substitutes internal/external entities into the tree, which on this attacker-facing X1
    // interface is an XXE/entity-expansion vector (e.g. an entity resolving file:///... into a
    // TargetIdentifier value the POI would store and log). X1 messages have no legitimate entity
    // use, and schema validation rejects anything the parser leaves as an unexpanded reference.
    xmlDocPtr doc =
        xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "x1.xml", nullptr, XML_PARSE_NONET);
    if (doc == nullptr) {
        return tl::make_unexpected(ParseError{true, "not well-formed XML", std::nullopt});
    }
    struct DocGuard {
        xmlDocPtr d;
        ~DocGuard() { xmlFreeDoc(d); }
    } guard{doc};

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root == nullptr || !is(root, "X1Request")) {
        return tl::make_unexpected(ParseError{true, "root is not X1Request", std::nullopt});
    }
    if (!schema_valid(doc)) {
        // Best-effort header for the TopLevelError response (6.1): identifiers if the first
        // message carries them.
        std::optional<MessageHeader> h;
        if (xmlNodePtr m = child(root, "x1RequestMessage")) {
            h = read_header(m);
        }
        return tl::make_unexpected(ParseError{true, "not schema-valid (TS 103 221-1 7.2.1)", h});
    }
    RequestContainer container;
    for (xmlNodePtr m = root->children; m != nullptr; m = m->next) {
        if (is(m, "x1RequestMessage")) {
            container.requests.push_back(read_request(m));
        }
    }
    return container;
}

namespace {

// Shared response-doc scaffolding.
struct ResponseDoc {
    xmlDocPtr doc;
    xmlNodePtr root;
    xmlNsPtr xsi;
    ResponseDoc() {
        doc = xmlNewDoc(reinterpret_cast<const xmlChar*>("1.0"));
        root = xmlNewNode(nullptr, reinterpret_cast<const xmlChar*>("X1Response"));
        xmlNsPtr x1 = xmlNewNs(root, reinterpret_cast<const xmlChar*>(kX1Ns), nullptr);
        xmlSetNs(root, x1);
        xsi = xmlNewNs(root,
                       reinterpret_cast<const xmlChar*>(kXsiNs),
                       reinterpret_cast<const xmlChar*>("xsi"));
        xmlNewNs(
            root, reinterpret_cast<const xmlChar*>(kX1Ns), reinterpret_cast<const xmlChar*>("x1"));
        xmlDocSetRootElement(doc, root);
    }
    ~ResponseDoc() { xmlFreeDoc(doc); }
    xmlNodePtr message(const MessageHeader& h, const char* xsi_type_name) {
        xmlNodePtr m = xmlNewChild(
            root, root->ns, reinterpret_cast<const xmlChar*>("x1ResponseMessage"), nullptr);
        set_ns_type(m, xsi, xsi_type_name);
        write_header(m, h);
        return m;
    }
    std::string dump() const {
        xmlChar* buf = nullptr;
        int len = 0;
        xmlDocDumpFormatMemoryEnc(doc, &buf, &len, "UTF-8", 1);
        std::string out(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(len));
        xmlFree(buf);
        return out;
    }
};

} // namespace

tl::expected<std::string, std::string> serialise_response(const std::vector<ResponseItem>& items) {
    ResponseDoc rd;
    for (const auto& item : items) {
        std::visit(
            [&](const auto& r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, OkResponse>) {
                    xmlNodePtr m = rd.message(r.header, response_xsi_type(r.type));
                    add_text(m,
                             "oK",
                             r.acknowledged_and_completed ? "AcknowledgedAndCompleted"
                                                          : "Acknowledged");
                } else if constexpr (std::is_same_v<T, ErrorResponse>) {
                    xmlNodePtr m = rd.message(r.header, "ErrorResponse");
                    add_text(m, "requestMessageType", message_type_name(r.type));
                    xmlNodePtr ei = xmlNewChild(
                        m, m->ns, reinterpret_cast<const xmlChar*>("errorInformation"), nullptr);
                    add_text(ei, "errorCode", std::to_string(static_cast<int>(r.code)));
                    add_text(ei, "errorDescription", r.description);
                }
            },
            item);
    }
    const std::string xml = rd.dump();
    xmlDocPtr check =
        xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "r.xml", nullptr, XML_PARSE_NONET);
    if (check == nullptr) {
        return tl::make_unexpected("built response is not well-formed");
    }
    const bool ok = schema_valid(check);
    xmlFreeDoc(check);
    if (!ok) {
        return tl::make_unexpected("built response failed schema validation");
    }
    return xml;
}

std::string serialise_top_level_error(const MessageHeader& header) {
    xmlDocPtr doc = xmlNewDoc(reinterpret_cast<const xmlChar*>("1.0"));
    xmlNodePtr root =
        xmlNewNode(nullptr, reinterpret_cast<const xmlChar*>("X1TopLevelErrorResponse"));
    xmlNsPtr x1 = xmlNewNs(root, reinterpret_cast<const xmlChar*>(kX1Ns), nullptr);
    xmlSetNs(root, x1);
    xmlDocSetRootElement(doc, root);
    write_header(root, header);
    xmlChar* buf = nullptr;
    int len = 0;
    xmlDocDumpFormatMemoryEnc(doc, &buf, &len, "UTF-8", 1);
    std::string out(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(len));
    xmlFree(buf);
    xmlFreeDoc(doc);
    return out;
}

// ---- ADMF (client) side, ADR-0462 ----------------------------------------------------------

namespace {

const char* delivery_text(DeliveryType d) {
    switch (d) {
        case DeliveryType::X2Only:
            return "X2Only";
        case DeliveryType::X3Only:
            return "X3Only";
        case DeliveryType::X2AndX3:
            return "X2andX3";
    }
    return "X2andX3";
}

const char* mediation_delivery_text(MediationDeliveryType d) {
    switch (d) {
        case MediationDeliveryType::Hi2Only:
            return "HI2Only";
        case MediationDeliveryType::Hi3Only:
            return "HI3Only";
        case MediationDeliveryType::Hi2AndHi3:
            return "HI2andHI3";
    }
    return "HI2andHI3";
}

xmlNodePtr add_node(xmlNodePtr parent, const char* name) {
    return xmlNewChild(parent, parent->ns, reinterpret_cast<const xmlChar*>(name), nullptr);
}

void add_dids(xmlNodePtr parent,
              const std::vector<std::string>& dids,
              const std::vector<std::string>& dsids) {
    xmlNodePtr list = add_node(parent, "listOfDIDs");
    for (const auto& d : dids) {
        add_text(list, "dId", d);
    }
    for (const auto& d : dsids) {
        add_text(list, "dSId", d);
    }
}

// The XSD choice element for a target identifier: the kind's own element, or the verbatim
// `element` of a Kind::Other identifier.
std::string target_element(const TargetIdentifier& t) {
    if (!t.element.empty()) {
        return t.element;
    }
    for (const auto& k : kTargetKinds) {
        if (k.kind == t.kind) {
            return k.element;
        }
    }
    return "";
}

void write_task_details(xmlNodePtr parent, const TaskDetails& t) {
    xmlNodePtr td = add_node(parent, "taskDetails");
    add_text(td, "xId", t.xid);
    xmlNodePtr targets = add_node(td, "targetIdentifiers");
    for (const auto& target : t.targets) {
        xmlNodePtr ti = add_node(targets, "targetIdentifier");
        add_text(ti, target_element(target).c_str(), target.value);
    }
    add_text(td, "deliveryType", delivery_text(t.delivery));
    add_dids(td, t.dids, t.dsids);
    if (!t.mediation_details.empty()) {
        xmlNodePtr list = add_node(td, "listOfMediationDetails");
        for (const auto& m : t.mediation_details) {
            xmlNodePtr md = add_node(list, "mediationDetails");
            add_text(md, "LIID", m.liid);
            add_text(md, "deliveryType", mediation_delivery_text(m.delivery));
            if (m.start_time) {
                add_text(md, "StartTime", *m.start_time);
            }
            if (m.end_time) {
                add_text(md, "EndTime", *m.end_time);
            }
            if (!m.dids.empty()) {
                add_dids(md, m.dids, {});
            }
        }
    }
    if (t.correlation_id) {
        add_text(td, "correlationID", std::to_string(*t.correlation_id));
    }
    if (t.implicit_deactivation_allowed) {
        add_text(
            td, "implicitDeactivationAllowed", *t.implicit_deactivation_allowed ? "true" : "false");
    }
    if (t.product_id) {
        add_text(td, "productID", *t.product_id);
    }
    if (t.identifier_association_events) {
        // TS 33.128 table 6.2.2.1.1-1 (see read_task_gating): an ETSI Extension, Owner + the 3GPP
        // X1 extension element.
        xmlNodePtr ext = add_node(td, "taskDetailsExtensions");
        add_text(ext, "Owner", "3GPP");
        xmlNsPtr tgpp = xmlNewNs(ext,
                                 reinterpret_cast<const xmlChar*>(k3gppX1ExtNs),
                                 reinterpret_cast<const xmlChar*>("tgpp"));
        xmlNodePtr ia =
            xmlNewChild(ext,
                        tgpp,
                        reinterpret_cast<const xmlChar*>("IdentifierAssociationExtensions"),
                        nullptr);
        xmlNewTextChild(
            ia,
            tgpp,
            reinterpret_cast<const xmlChar*>("IdentifierAssociationEventsGenerated"),
            reinterpret_cast<const xmlChar*>(*t.identifier_association_events ==
                                                     IdentifierAssociationEventsGenerated::All
                                                 ? "All"
                                                 : "IdentifierAssociation"));
    }
}

// TS 103 280 IPv6Address is the fixed form `([0-9a-f]{4}:){7}([0-9a-f]{4})` -- no `::`
// compression, no upper case -- so any textual IPv6 is expanded to it. Anything inet_pton cannot
// parse is passed through unchanged, so the schema check rejects it rather than this guessing.
std::string expand_ipv6(const std::string& text) {
    in6_addr addr{};
    if (inet_pton(AF_INET6, text.c_str(), &addr) != 1) {
        return text;
    }
    char buf[48];
    std::snprintf(buf,
                  sizeof(buf),
                  "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                  addr.s6_addr[0],
                  addr.s6_addr[1],
                  addr.s6_addr[2],
                  addr.s6_addr[3],
                  addr.s6_addr[4],
                  addr.s6_addr[5],
                  addr.s6_addr[6],
                  addr.s6_addr[7],
                  addr.s6_addr[8],
                  addr.s6_addr[9],
                  addr.s6_addr[10],
                  addr.s6_addr[11],
                  addr.s6_addr[12],
                  addr.s6_addr[13],
                  addr.s6_addr[14],
                  addr.s6_addr[15]);
    return buf;
}

void write_destination(xmlNodePtr parent, const DestinationDetails& d, xmlNsPtr c) {
    xmlNodePtr dd = add_node(parent, "destinationDetails");
    add_text(dd, "dId", d.did);
    if (d.friendly_name) {
        add_text(dd, "friendlyName", *d.friendly_name);
    }
    add_text(dd, "deliveryType", delivery_text(d.delivery));
    xmlNodePtr addr = add_node(dd, "deliveryAddress");
    switch (d.address.kind) {
        case DeliveryAddress::Kind::IpAddressAndPort: {
            // IPAddressPort's children are TS 103 280 elements (elementFormDefault=qualified), so
            // they carry that namespace, not the X1 one.
            const auto colon = d.address.value.rfind(':');
            const std::string ip =
                colon == std::string::npos ? d.address.value : d.address.value.substr(0, colon);
            const std::string port =
                colon == std::string::npos ? "" : d.address.value.substr(colon + 1);
            const bool v6 = ip.find(':') != std::string::npos;
            xmlNodePtr ipp = add_node(addr, "ipAddressAndPort");
            xmlNodePtr a =
                xmlNewChild(ipp, c, reinterpret_cast<const xmlChar*>("address"), nullptr);
            xmlNewTextChild(a,
                            c,
                            reinterpret_cast<const xmlChar*>(v6 ? "IPv6Address" : "IPv4Address"),
                            reinterpret_cast<const xmlChar*>((v6 ? expand_ipv6(ip) : ip).c_str()));
            xmlNodePtr p = xmlNewChild(ipp, c, reinterpret_cast<const xmlChar*>("port"), nullptr);
            xmlNewTextChild(p,
                            c,
                            reinterpret_cast<const xmlChar*>("TCPPort"),
                            reinterpret_cast<const xmlChar*>(port.c_str()));
            break;
        }
        case DeliveryAddress::Kind::E164Number:
            add_text(addr, "e164Number", d.address.value);
            break;
        case DeliveryAddress::Kind::Uri:
            add_text(addr, "uri", d.address.value);
            break;
        case DeliveryAddress::Kind::EmailAddress:
            add_text(addr, "emailAddress", d.address.value);
            break;
    }
}

} // namespace

tl::expected<std::string, std::string> serialise_request(const std::vector<Request>& requests) {
    if (requests.empty()) {
        return tl::make_unexpected(std::string("an X1Request carries at least one request (6.1)"));
    }
    xmlDocPtr doc = xmlNewDoc(reinterpret_cast<const xmlChar*>("1.0"));
    struct DocGuard {
        xmlDocPtr d;
        ~DocGuard() { xmlFreeDoc(d); }
    } guard{doc};
    xmlNodePtr root = xmlNewNode(nullptr, reinterpret_cast<const xmlChar*>("X1Request"));
    xmlNsPtr x1 = xmlNewNs(root, reinterpret_cast<const xmlChar*>(kX1Ns), nullptr);
    xmlSetNs(root, x1);
    xmlNsPtr xsi = xmlNewNs(
        root, reinterpret_cast<const xmlChar*>(kXsiNs), reinterpret_cast<const xmlChar*>("xsi"));
    xmlNewNs(root, reinterpret_cast<const xmlChar*>(kX1Ns), reinterpret_cast<const xmlChar*>("x1"));
    xmlNsPtr c =
        xmlNewNs(root,
                 reinterpret_cast<const xmlChar*>("http://uri.etsi.org/03280/common/2017/07"),
                 reinterpret_cast<const xmlChar*>("c"));
    xmlDocSetRootElement(doc, root);

    for (const auto& req : requests) {
        const char* xsi_name = nullptr;
        for (const auto& t : kTypes) {
            if (t.type == req.type) {
                xsi_name = t.xsi;
            }
        }
        if (xsi_name == nullptr || req.type == MessageType::ReportTaskIssue ||
            req.type == MessageType::ReportNEIssue) {
            return tl::make_unexpected(std::string("request type is not buildable by an ADMF: ") +
                                       message_type_name(req.type));
        }
        xmlNodePtr m =
            xmlNewChild(root, x1, reinterpret_cast<const xmlChar*>("x1RequestMessage"), nullptr);
        set_ns_type(m, xsi, xsi_name);
        write_header(m, req.header);
        switch (req.type) {
            case MessageType::ActivateTask:
                write_task_details(m, std::get<ActivateTask>(req.body).task);
                break;
            case MessageType::ModifyTask:
                write_task_details(m, std::get<ModifyTask>(req.body).task);
                break;
            case MessageType::DeactivateTask:
                add_text(m, "xId", std::get<DeactivateTask>(req.body).xid);
                break;
            case MessageType::GetTaskDetails:
                add_text(m, "xId", std::get<GetTaskDetails>(req.body).xid);
                break;
            case MessageType::CreateDestination:
                write_destination(m, std::get<CreateDestination>(req.body).destination, c);
                break;
            case MessageType::RemoveDestination:
                add_text(m, "dId", std::get<RemoveDestination>(req.body).did);
                break;
            default: // DeactivateAllTasks, RemoveAllDestinations, Ping, Keepalive: header only
                break;
        }
    }

    xmlChar* buf = nullptr;
    int len = 0;
    xmlDocDumpFormatMemoryEnc(doc, &buf, &len, "UTF-8", 1);
    std::string xml(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(len));
    xmlFree(buf);
    if (!schema_valid(doc)) {
        return tl::make_unexpected(std::string("built request failed schema validation"));
    }
    return xml;
}

namespace {

MessageType type_from_name(const std::string& name) {
    for (const auto& t : kTypes) {
        if (name == message_type_name(t.type)) {
            return t.type;
        }
    }
    return MessageType::Unsupported;
}

ProvisioningStatus read_provisioning(const std::string& v) {
    if (v == "complete") {
        return ProvisioningStatus::Complete;
    }
    if (v == "failed") {
        return ProvisioningStatus::Failed;
    }
    return ProvisioningStatus::AwaitingProvisioning;
}

} // namespace

tl::expected<std::vector<ClientResponse>, ParseError> parse_response(const std::string& xml) {
    // Same hardening as parse_request: no network, no entity substitution.
    xmlDocPtr doc = xmlReadMemory(
        xml.data(), static_cast<int>(xml.size()), "x1r.xml", nullptr, XML_PARSE_NONET);
    if (doc == nullptr) {
        return tl::make_unexpected(ParseError{true, "not well-formed XML", std::nullopt});
    }
    struct DocGuard {
        xmlDocPtr d;
        ~DocGuard() { xmlFreeDoc(d); }
    } guard{doc};
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root == nullptr) {
        return tl::make_unexpected(ParseError{true, "empty document", std::nullopt});
    }
    if (is(root, "X1TopLevelErrorResponse")) {
        return tl::make_unexpected(
            ParseError{true, "NE answered with an X1TopLevelErrorResponse", read_header(root)});
    }
    if (!is(root, "X1Response")) {
        return tl::make_unexpected(ParseError{true, "root is not X1Response", std::nullopt});
    }
    if (!schema_valid(doc)) {
        return tl::make_unexpected(
            ParseError{true, "response is not schema-valid (TS 103 221-1 7.2.1)", std::nullopt});
    }
    std::vector<ClientResponse> out;
    for (xmlNodePtr m = root->children; m != nullptr; m = m->next) {
        if (!is(m, "x1ResponseMessage")) {
            continue;
        }
        const MessageHeader header = read_header(m);
        const std::string xt = xsi_type(m);
        if (xt == "ErrorResponse") {
            ErrorResponse e;
            e.header = header;
            e.type = type_from_name(child_text(m, "requestMessageType").value_or(""));
            if (xmlNodePtr ei = child(m, "errorInformation")) {
                e.code = static_cast<ErrorCode>(
                    std::atoi(child_text(ei, "errorCode").value_or("0").c_str()));
                e.description = child_text(ei, "errorDescription").value_or("");
            }
            out.emplace_back(std::move(e));
        } else if (xt == "GetTaskDetailsResponse") {
            TaskDetailsResponse r;
            r.header = header;
            if (xmlNodePtr trd = child(m, "taskResponseDetails")) {
                if (xmlNodePtr td = child(trd, "taskDetails")) {
                    r.task = read_task_details(td);
                }
                if (xmlNodePtr st = child(trd, "taskStatus")) {
                    r.provisioning =
                        read_provisioning(child_text(st, "provisioningStatus").value_or(""));
                    if (xmlNodePtr faults = child(st, "listOfFaults")) {
                        for (xmlNodePtr f = faults->children; f != nullptr; f = f->next) {
                            if (is(f, "unresolvedFault")) {
                                r.unresolved_faults.push_back(Fault{
                                    std::atoi(child_text(f, "errorCode").value_or("0").c_str()),
                                    child_text(f, "errorDescription").value_or("")});
                            }
                        }
                    }
                }
            }
            out.emplace_back(std::move(r));
        } else if (auto ok = child_text(m, "oK")) {
            OkResponse r;
            r.header = header;
            std::string base = xt;
            if (base.size() > 8 && base.compare(base.size() - 8, 8, "Response") == 0) {
                base.resize(base.size() - 8);
            }
            r.type = type_from_name(base);
            r.acknowledged_and_completed = (*ok == "AcknowledgedAndCompleted");
            out.emplace_back(std::move(r));
        } else {
            return tl::make_unexpected(
                ParseError{false, "unsupported response type " + xt, header});
        }
    }
    return out;
}

} // namespace li_core::x1
