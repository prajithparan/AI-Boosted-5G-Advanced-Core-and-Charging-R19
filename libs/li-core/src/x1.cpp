#include "li_core/x1.hpp"

#include <array>
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
    if (auto c = child_text(td, "correlationID")) {
        task.correlation_id = std::strtoull(c->c_str(), nullptr, 10);
    }
    if (auto c = child_text(td, "implicitDeactivationAllowed")) {
        task.implicit_deactivation_allowed = (*c == "true" || *c == "1");
    }
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
            // TS 103 280 IPAddressAndPort: address + port children.
            std::string a;
            if (xmlNodePtr addrn = child(ip, "address")) {
                if (xmlNodePtr v4 = child(addrn, "iPv4Address")) {
                    a = node_text(v4);
                } else if (xmlNodePtr v6 = child(addrn, "iPv6Address")) {
                    a = node_text(v6);
                }
            }
            dest.address.value = a + ":" + child_text(ip, "port").value_or("");
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
constexpr std::array<TypeMap, 10> kTypes{{
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
}};

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

} // namespace li_core::x1
