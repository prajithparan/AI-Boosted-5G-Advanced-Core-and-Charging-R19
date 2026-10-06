#include "li_core/hi1.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlschemas.h>
#include <map>
#include <mutex>
#include <utility>

// ETSI TS 103 120 LI_HI1 XML codec over libxml2 (ADR-0462 step 2). libxml2 stays inside li_core's
// shared object -- no li_core:: header names a libxml2 type. See hi1.hpp for the design (objects
// held as self-contained XML, typed reads, ordered edits, every emitted message schema-validated).

namespace li_core::hi1 {

namespace {

constexpr const char* kCoreNs = "http://uri.etsi.org/03120/common/2019/10/Core";
constexpr const char* kCommonNs = "http://uri.etsi.org/03120/common/2016/02/Common";
constexpr const char* kAuthNs = "http://uri.etsi.org/03120/common/2020/09/Authorisation";
constexpr const char* kTaskNs = "http://uri.etsi.org/03120/common/2020/09/Task";
constexpr const char* kDocNs = "http://uri.etsi.org/03120/common/2020/09/Document";
constexpr const char* kNotifNs = "http://uri.etsi.org/03120/common/2016/02/Notification";
constexpr const char* kConfigNs = "http://uri.etsi.org/03120/common/2024/06/Config";
constexpr const char* kXsiNs = "http://www.w3.org/2001/XMLSchema-instance";

const xmlChar* X(const char* s) {
    return reinterpret_cast<const xmlChar*>(s);
}
const char* C(const xmlChar* s) {
    return reinterpret_cast<const char*>(s);
}

// ---- schema -----------------------------------------------------------------------------------
// One schema, loaded once (the wrapper that wires every import). xmlSchemaValidateDoc mutates the
// schema's compiled-type cache, so validations are serialised (same reasoning as x1.cpp).
xmlSchemaPtr load_schema() {
    static std::once_flag once;
    static xmlSchemaPtr schema = nullptr;
    std::call_once(once, [] {
        xmlInitParser();
        const std::string path = std::string(LI_ETSI_SCHEMA_DIR) + "/103120/hi1-validation.xsd";
        if (xmlSchemaParserCtxtPtr pc = xmlSchemaNewParserCtxt(path.c_str())) {
            schema = xmlSchemaParse(pc);
            xmlSchemaFreeParserCtxt(pc);
        }
    });
    return schema;
}

void capture_error(void* ctx, const xmlError* error) {
    auto* out = static_cast<std::string*>(ctx);
    if (out->empty() && error != nullptr && error->message != nullptr) {
        *out = error->message;
        while (!out->empty() && (out->back() == '\n' || out->back() == '\r')) {
            out->pop_back();
        }
    }
}

bool schema_valid(xmlDocPtr doc, std::string* why) {
    xmlSchemaPtr schema = load_schema();
    if (schema == nullptr) {
        if (why != nullptr) {
            *why = "the HI1 schema set is not available";
        }
        return false; // no schema on disk -> cannot claim validity
    }
    static std::mutex kSchemaMutex;
    const std::lock_guard<std::mutex> lock(kSchemaMutex);
    xmlSchemaValidCtxtPtr vc = xmlSchemaNewValidCtxt(schema);
    std::string first_error;
    xmlSchemaSetValidStructuredErrors(vc, capture_error, &first_error);
    const int rc = xmlSchemaValidateDoc(vc, doc);
    xmlSchemaFreeValidCtxt(vc);
    if (rc != 0 && why != nullptr) {
        *why = first_error.empty() ? "not schema-valid" : first_error;
    }
    return rc == 0;
}

// ---- small xml helpers --------------------------------------------------------------------------
struct Doc {
    xmlDocPtr d = nullptr;
    Doc() = default;
    explicit Doc(xmlDocPtr doc) : d(doc) {}
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    Doc(Doc&& o) noexcept : d(o.d) { o.d = nullptr; }
    ~Doc() {
        if (d != nullptr) {
            xmlFreeDoc(d);
        }
    }
    explicit operator bool() const { return d != nullptr; }
};

// NONET on, NOENT deliberately off: no network fetches, no entity substitution (XXE hardening,
// as in x1.cpp). BLANKS dropped so a pretty-printed document has no whitespace text nodes.
Doc parse_xml(const std::string& xml) {
    return Doc(xmlReadMemory(xml.data(),
                             static_cast<int>(xml.size()),
                             "hi1.xml",
                             nullptr,
                             XML_PARSE_NONET | XML_PARSE_NOBLANKS));
}

bool is(xmlNodePtr n, const char* local) {
    return n != nullptr && n->type == XML_ELEMENT_NODE && xmlStrcmp(n->name, X(local)) == 0;
}

xmlNodePtr child(xmlNodePtr parent, const char* local) {
    if (parent == nullptr) {
        return nullptr;
    }
    for (xmlNodePtr n = parent->children; n != nullptr; n = n->next) {
        if (is(n, local)) {
            return n;
        }
    }
    return nullptr;
}

std::string text_of(xmlNodePtr n) {
    xmlChar* c = xmlNodeGetContent(n);
    std::string out = c != nullptr ? C(c) : "";
    xmlFree(c);
    return out;
}

std::optional<std::string> child_text(xmlNodePtr parent, const char* local) {
    if (xmlNodePtr n = child(parent, local)) {
        return text_of(n);
    }
    return std::nullopt;
}

std::optional<DictionaryEntry> read_entry(xmlNodePtr n) {
    if (n == nullptr) {
        return std::nullopt;
    }
    DictionaryEntry e;
    e.owner = child_text(n, "Owner").value_or("");
    e.name = child_text(n, "Name").value_or("");
    e.value = child_text(n, "Value").value_or("");
    return e;
}

std::optional<std::uint64_t> to_u64(const std::optional<std::string>& s) {
    if (!s || s->empty()) {
        return std::nullopt;
    }
    return std::strtoull(s->c_str(), nullptr, 10);
}

std::string dump_node(xmlDocPtr doc, xmlNodePtr node) {
    xmlBufferPtr buf = xmlBufferCreate();
    xmlNodeDump(buf, doc, node, 0, 0);
    std::string out = buf->content != nullptr ? C(buf->content) : "";
    xmlBufferFree(buf);
    return out;
}

std::string dump_doc(xmlDocPtr doc) {
    xmlChar* buf = nullptr;
    int len = 0;
    xmlDocDumpFormatMemoryEnc(doc, &buf, &len, "UTF-8", 1);
    std::string out(C(buf), static_cast<std::size_t>(len));
    xmlFree(buf);
    return out;
}

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

// ---- object type tables -------------------------------------------------------------------------
// The XSD sequence order of each object type's members (ts_103120_Core.xsd HI1Object, then the
// type's own extension sequence). set_* uses these to place a member where the schema wants it.
constexpr std::array kBaseOrder{std::string_view("ObjectIdentifier"),
                                std::string_view("CountryCode"),
                                std::string_view("OwnerIdentifier"),
                                std::string_view("Generation"),
                                std::string_view("ExternalIdentifier"),
                                std::string_view("AssociatedObjects"),
                                std::string_view("LastChanged"),
                                std::string_view("NationalHandlingParameters")};
constexpr std::array kAuthOrder{std::string_view("AuthorisationReference"),
                                std::string_view("AuthorisationLegalType"),
                                std::string_view("AuthorisationPriority"),
                                std::string_view("AuthorisationStatus"),
                                std::string_view("AuthorisationDesiredStatus"),
                                std::string_view("AuthorisationTimespan"),
                                std::string_view("AuthorisationCSPID"),
                                std::string_view("AuthorisationCreationTimestamp"),
                                std::string_view("AuthorisationServedTimestamp"),
                                std::string_view("AuthorisationTerminationTimestamp"),
                                std::string_view("AuthorisationApprovalDetails"),
                                std::string_view("AuthorisationInvalidReason"),
                                std::string_view("AuthorisationFlags"),
                                std::string_view("AuthorisationManualInformation"),
                                std::string_view("NationalAuthorisationParameters"),
                                std::string_view("AuthorisationJurisdiction"),
                                std::string_view("AuthorisationTypeOfCase"),
                                std::string_view("AuthorisationLegalEntity")};
constexpr std::array kTaskOrder{std::string_view("Reference"),
                                std::string_view("Status"),
                                std::string_view("DesiredStatus"),
                                std::string_view("Timespan"),
                                std::string_view("TargetIdentifier"),
                                std::string_view("DeliveryType"),
                                std::string_view("DeliveryDetails"),
                                std::string_view("ApprovalDetails"),
                                std::string_view("CSPID"),
                                std::string_view("HandlingProfile"),
                                std::string_view("InvalidReason"),
                                std::string_view("Flags"),
                                std::string_view("NationalLITaskingParameters"),
                                std::string_view("ListOfTrafficPolicyReferences"),
                                std::string_view("ListOfIRIPolicyReferences")};
constexpr std::array kDocOrder{std::string_view("DocumentReference"),
                               std::string_view("DocumentName"),
                               std::string_view("DocumentStatus"),
                               std::string_view("DocumentDesiredStatus"),
                               std::string_view("DocumentTimespan"),
                               std::string_view("DocumentType"),
                               std::string_view("DocumentProperties"),
                               std::string_view("DocumentBody"),
                               std::string_view("DocumentSignature"),
                               std::string_view("DocumentInvalidReason"),
                               std::string_view("NationalDocumentParameters")};
constexpr std::array kNotifOrder{std::string_view("NotificationDetails"),
                                 std::string_view("NotificationType"),
                                 std::string_view("NewNotification"),
                                 std::string_view("NotificationTimestamp"),
                                 std::string_view("StatusOfAssociatedObjects"),
                                 std::string_view("NationalNotificationParameters")};

struct TypeInfo {
    const char* type_name;
    const char* ns;
    const char* prefix;
    ObjectType type;
    const std::string_view* order;
    std::size_t order_size;
};
const TypeInfo kTypes[] = {
    {"AuthorisationObject", kAuthNs, "auth", ObjectType::Authorisation, kAuthOrder.data(), kAuthOrder.size()},
    {"LITaskObject", kTaskNs, "task", ObjectType::LITask, kTaskOrder.data(), kTaskOrder.size()},
    {"DocumentObject", kDocNs, "doc", ObjectType::Document, kDocOrder.data(), kDocOrder.size()},
    {"NotificationObject", kNotifNs, "notif", ObjectType::Notification, kNotifOrder.data(), kNotifOrder.size()},
};

const TypeInfo* type_info_for(ObjectType t) {
    for (const auto& ti : kTypes) {
        if (ti.type == t) {
            return &ti;
        }
    }
    return nullptr;
}

// The well-known prefixes this codec itself writes (a document it generates declares exactly
// these). Parsed documents may use any prefix; reads match on local name.
struct KnownNs {
    const char* prefix;
    const char* uri;
};
constexpr KnownNs kKnown[] = {
    {"xsi", kXsiNs},
    {"common", kCommonNs},
    {"auth", kAuthNs},
    {"task", kTaskNs},
    {"doc", kDocNs},
    {"notif", kNotifNs},
    {"config", kConfigNs},
};

// Declare `uri` on `root` if nothing in scope already maps to it; returns the ns either way.
xmlNsPtr ensure_ns(xmlDocPtr doc, xmlNodePtr root, const char* uri, const char* prefix) {
    if (xmlNsPtr ns = xmlSearchNsByHref(doc, root, X(uri))) {
        return ns;
    }
    return xmlNewNs(root, X(uri), prefix != nullptr && *prefix != '\0' ? X(prefix) : nullptr);
}

// Resolve an xsi:type QName on `node` to {namespace uri, local name}.
std::optional<std::pair<std::string, std::string>> resolve_xsi_type(xmlDocPtr doc, xmlNodePtr node) {
    xmlChar* t = xmlGetNsProp(node, X("type"), X(kXsiNs));
    if (t == nullptr) {
        return std::nullopt;
    }
    std::string v = C(t);
    xmlFree(t);
    std::string prefix;
    std::string local = v;
    if (const auto colon = v.find(':'); colon != std::string::npos) {
        prefix = v.substr(0, colon);
        local = v.substr(colon + 1);
    }
    xmlNsPtr ns = xmlSearchNs(doc, node, prefix.empty() ? nullptr : X(prefix.c_str()));
    return std::make_pair(ns != nullptr && ns->href != nullptr ? std::string(C(ns->href)) : "", local);
}

// ---- Object -------------------------------------------------------------------------------------

} // namespace

tl::expected<Object, std::string> Object::from_xml(std::string xml) {
    Doc doc = parse_xml(xml);
    if (!doc) {
        return tl::make_unexpected(std::string("HI1Object is not well-formed XML"));
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    if (!is(root, "HI1Object")) {
        return tl::make_unexpected(std::string("root element is not HI1Object"));
    }
    Object out;
    out.identifier_ = child_text(root, "ObjectIdentifier").value_or("");
    if (out.identifier_.empty()) {
        return tl::make_unexpected(std::string("HI1Object has no ObjectIdentifier"));
    }
    const auto type = resolve_xsi_type(doc.d, root);
    if (!type) {
        return tl::make_unexpected(std::string("HI1Object has no xsi:type"));
    }
    out.type_name_ = type->second;
    out.type_ = ObjectType::Other;
    for (const auto& ti : kTypes) {
        if (type->second == ti.type_name && type->first == ti.ns) {
            out.type_ = ti.type;
        }
    }
    out.xml_ = std::move(xml);
    return out;
}

std::optional<std::string> Object::text(std::string_view field) const {
    Doc doc = parse_xml(xml_);
    return doc ? child_text(xmlDocGetRootElement(doc.d), std::string(field).c_str()) : std::nullopt;
}

std::optional<DictionaryEntry> Object::entry(std::string_view field) const {
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return std::nullopt;
    }
    return read_entry(child(xmlDocGetRootElement(doc.d), std::string(field).c_str()));
}

std::optional<std::string> Object::text_at(const std::vector<std::string>& path) const {
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return std::nullopt;
    }
    xmlNodePtr n = xmlDocGetRootElement(doc.d);
    for (const auto& step : path) {
        n = child(n, step.c_str());
        if (n == nullptr) {
            return std::nullopt;
        }
    }
    return text_of(n);
}

std::vector<std::string> Object::associated_objects() const {
    std::vector<std::string> out;
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return out;
    }
    if (xmlNodePtr list = child(xmlDocGetRootElement(doc.d), "AssociatedObjects")) {
        for (xmlNodePtr n = list->children; n != nullptr; n = n->next) {
            if (is(n, "AssociatedObject")) {
                out.push_back(text_of(n));
            }
        }
    }
    return out;
}

namespace {

// Where `field` lives for this object: its index in the full member order (base members first)
// and the namespace its element belongs to. nullopt if the type has no such member.
struct FieldSlot {
    std::size_t index = 0;
    const char* ns = nullptr;
    const char* prefix = nullptr;
};
std::optional<FieldSlot> slot_of(ObjectType type, std::string_view field) {
    for (std::size_t i = 0; i < kBaseOrder.size(); ++i) {
        if (kBaseOrder[i] == field) {
            return FieldSlot{i, kCoreNs, ""};
        }
    }
    if (const TypeInfo* ti = type_info_for(type)) {
        for (std::size_t i = 0; i < ti->order_size; ++i) {
            if (ti->order[i] == field) {
                return FieldSlot{kBaseOrder.size() + i, ti->ns, ti->prefix};
            }
        }
    }
    return std::nullopt;
}

// Position of a child element within the member order (npos if unknown / not a member).
std::size_t index_of_child(ObjectType type, xmlNodePtr n) {
    if (n->type != XML_ELEMENT_NODE) {
        return static_cast<std::size_t>(-1);
    }
    const auto slot = slot_of(type, C(n->name));
    return slot ? slot->index : static_cast<std::size_t>(-1);
}

// Replace (or insert, in schema order) the member `field` of the HI1Object `root` with `fresh`.
void place_member(ObjectType type, xmlNodePtr root, xmlNodePtr fresh, std::size_t index) {
    for (xmlNodePtr n = root->children; n != nullptr; n = n->next) {
        if (n->type == XML_ELEMENT_NODE && xmlStrcmp(n->name, fresh->name) == 0) {
            xmlNodePtr old = xmlReplaceNode(n, fresh);
            xmlFreeNode(old);
            return;
        }
    }
    for (xmlNodePtr n = root->children; n != nullptr; n = n->next) {
        const std::size_t i = index_of_child(type, n);
        if (i != static_cast<std::size_t>(-1) && i > index) {
            xmlAddPrevSibling(n, fresh);
            return;
        }
    }
    xmlAddChild(root, fresh);
}

} // namespace

tl::expected<void, std::string> Object::set_text(std::string_view field, const std::string& value) {
    const auto slot = slot_of(type_, field);
    if (!slot) {
        return tl::make_unexpected("object type " + type_name_ + " has no member " + std::string(field));
    }
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return tl::make_unexpected(std::string("stored object is not well-formed"));
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    xmlNsPtr ns = ensure_ns(doc.d, root, slot->ns, slot->prefix);
    xmlNodePtr fresh = xmlNewDocNode(doc.d, ns, X(std::string(field).c_str()), nullptr);
    xmlChar* enc = xmlEncodeSpecialChars(doc.d, X(value.c_str()));
    xmlNodeSetContent(fresh, enc);
    xmlFree(enc);
    place_member(type_, root, fresh, slot->index);
    xml_ = dump_node(doc.d, root);
    if (field == "ObjectIdentifier") {
        identifier_ = value;
    }
    return {};
}

tl::expected<void, std::string> Object::set_entry(std::string_view field, const DictionaryEntry& e) {
    const auto slot = slot_of(type_, field);
    if (!slot) {
        return tl::make_unexpected("object type " + type_name_ + " has no member " + std::string(field));
    }
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return tl::make_unexpected(std::string("stored object is not well-formed"));
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    xmlNsPtr ns = ensure_ns(doc.d, root, slot->ns, slot->prefix);
    xmlNsPtr common = ensure_ns(doc.d, root, kCommonNs, "common");
    xmlNodePtr fresh = xmlNewDocNode(doc.d, ns, X(std::string(field).c_str()), nullptr);
    const auto add = [&](const char* name, const std::string& v) {
        xmlNodePtr c = xmlNewDocNode(doc.d, common, X(name), nullptr);
        xmlChar* enc = xmlEncodeSpecialChars(doc.d, X(v.c_str()));
        xmlNodeSetContent(c, enc);
        xmlFree(enc);
        xmlAddChild(fresh, c);
    };
    add("Owner", e.owner);
    add("Name", e.name);
    add("Value", e.value);
    place_member(type_, root, fresh, slot->index);
    xml_ = dump_node(doc.d, root);
    return {};
}

tl::expected<void, std::string> Object::set_failure(std::string_view field,
                                                    std::uint32_t code,
                                                    const std::string& description) {
    const auto slot = slot_of(type_, field);
    if (!slot) {
        return tl::make_unexpected("object type " + type_name_ + " has no member " + std::string(field));
    }
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return tl::make_unexpected(std::string("stored object is not well-formed"));
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    xmlNsPtr ns = ensure_ns(doc.d, root, slot->ns, slot->prefix);
    xmlNsPtr core = ensure_ns(doc.d, root, kCoreNs, "");
    xmlNodePtr fresh = xmlNewDocNode(doc.d, ns, X(std::string(field).c_str()), nullptr);
    const auto add = [&](const char* name, const std::string& v) {
        xmlNodePtr c = xmlNewDocNode(doc.d, core, X(name), nullptr);
        xmlChar* enc = xmlEncodeSpecialChars(doc.d, X(v.c_str()));
        xmlNodeSetContent(c, enc);
        xmlFree(enc);
        xmlAddChild(fresh, c);
    };
    add("ErrorCode", std::to_string(code));
    add("ErrorDescription", description);
    place_member(type_, root, fresh, slot->index);
    xml_ = dump_node(doc.d, root);
    return {};
}

tl::expected<void, std::string> Object::set_associated_objects(const std::vector<std::string>& ids) {
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return tl::make_unexpected(std::string("stored object is not well-formed"));
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    xmlNsPtr ns = ensure_ns(doc.d, root, kCoreNs, "");
    xmlNodePtr fresh = xmlNewDocNode(doc.d, ns, X("AssociatedObjects"), nullptr);
    for (const auto& id : ids) {
        xmlNodePtr c = xmlNewDocNode(doc.d, ns, X("AssociatedObject"), nullptr);
        xmlChar* enc = xmlEncodeSpecialChars(doc.d, X(id.c_str()));
        xmlNodeSetContent(c, enc);
        xmlFree(enc);
        xmlAddChild(fresh, c);
    }
    place_member(type_, root, fresh, 5 /* AssociatedObjects */);
    xml_ = dump_node(doc.d, root);
    return {};
}

bool Object::remove(std::string_view field) {
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return false;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    xmlNodePtr n = child(root, std::string(field).c_str());
    if (n == nullptr) {
        return false;
    }
    xmlUnlinkNode(n);
    xmlFreeNode(n);
    xml_ = dump_node(doc.d, root);
    return true;
}

std::vector<std::string> Object::members() const {
    std::vector<std::string> out;
    Doc doc = parse_xml(xml_);
    if (!doc) {
        return out;
    }
    for (xmlNodePtr n = xmlDocGetRootElement(doc.d)->children; n != nullptr; n = n->next) {
        if (n->type == XML_ELEMENT_NODE) {
            out.emplace_back(C(n->name));
        }
    }
    return out;
}

tl::expected<void, std::string> Object::merge(const Object& update) {
    if (update.type_ != type_ || update.type_name_ != type_name_) {
        return tl::make_unexpected("an UPDATE must carry the same object type (" + type_name_ + ")");
    }
    Doc doc = parse_xml(xml_);
    Doc src = parse_xml(update.xml_);
    if (!doc || !src) {
        return tl::make_unexpected(std::string("object is not well-formed"));
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    for (xmlNodePtr n = xmlDocGetRootElement(src.d)->children; n != nullptr; n = n->next) {
        if (n->type != XML_ELEMENT_NODE || xmlStrcmp(n->name, X("ObjectIdentifier")) == 0) {
            continue;
        }
        const auto slot = slot_of(type_, C(n->name));
        if (!slot) {
            return tl::make_unexpected("object type " + type_name_ + " has no member " + C(n->name));
        }
        xmlNodePtr copy = xmlDocCopyNode(n, doc.d, 1);
        if (copy == nullptr) {
            return tl::make_unexpected(std::string("could not copy a member"));
        }
        place_member(type_, root, copy, slot->index);
    }
    xmlReconciliateNs(doc.d, root);
    xml_ = dump_node(doc.d, root);
    return {};
}

// ---- views --------------------------------------------------------------------------------------

namespace {

EndpointId read_endpoint(xmlNodePtr n) {
    return EndpointId{child_text(n, "CountryCode").value_or(""),
                      child_text(n, "UniqueIdentifier").value_or("")};
}

std::string read_ip_port(xmlNodePtr ipp) {
    std::string addr;
    if (xmlNodePtr a = child(ipp, "address")) {
        if (xmlNodePtr v4 = child(a, "IPv4Address")) {
            addr = text_of(v4);
        } else if (xmlNodePtr v6 = child(a, "IPv6Address")) {
            addr = text_of(v6);
        }
    }
    std::string port;
    if (xmlNodePtr p = child(ipp, "port")) {
        for (xmlNodePtr n = p->children; n != nullptr; n = n->next) {
            if (n->type == XML_ELEMENT_NODE) {
                port = text_of(n); // TCPPort or UDPPort
                break;
            }
        }
    }
    return addr + ":" + port;
}

} // namespace

AuthorisationView view_authorisation(const Object& object) {
    AuthorisationView v;
    Doc doc = parse_xml(object.xml());
    if (!doc) {
        return v;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    v.reference = child_text(root, "AuthorisationReference");
    v.status = read_entry(child(root, "AuthorisationStatus"));
    v.desired_status = read_entry(child(root, "AuthorisationDesiredStatus"));
    if (xmlNodePtr ts = child(root, "AuthorisationTimespan")) {
        v.start_time = child_text(ts, "StartTime");
        v.end_time = child_text(ts, "EndTime");
    }
    if (xmlNodePtr list = child(root, "AuthorisationCSPID")) {
        for (xmlNodePtr n = list->children; n != nullptr; n = n->next) {
            if (is(n, "CSPID")) {
                v.cspids.push_back(read_endpoint(n));
            }
        }
    }
    return v;
}

LiTaskView view_litask(const Object& object) {
    LiTaskView v;
    Doc doc = parse_xml(object.xml());
    if (!doc) {
        return v;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    v.liid = child_text(root, "Reference");
    v.status = read_entry(child(root, "Status"));
    v.desired_status = read_entry(child(root, "DesiredStatus"));
    if (xmlNodePtr ts = child(root, "Timespan")) {
        v.start_time = child_text(ts, "StartTime");
        v.end_time = child_text(ts, "EndTime");
    }
    if (xmlNodePtr ti = child(root, "TargetIdentifier")) {
        v.service_type = read_entry(child(ti, "ServiceType"));
        if (xmlNodePtr values = child(ti, "TargetIdentifierValues")) {
            for (xmlNodePtr n = values->children; n != nullptr; n = n->next) {
                if (!is(n, "TargetIdentifierValue")) {
                    continue;
                }
                TargetValue tv;
                if (xmlNodePtr ft = child(n, "FormatType")) {
                    tv.format_owner = child_text(ft, "FormatOwner").value_or("");
                    tv.format_name = child_text(ft, "FormatName").value_or("");
                }
                tv.value = child_text(n, "Value").value_or("");
                v.targets.push_back(std::move(tv));
            }
        }
    }
    v.delivery_type = read_entry(child(root, "DeliveryType"));
    if (xmlNodePtr dd = child(root, "DeliveryDetails")) {
        for (xmlNodePtr n = dd->children; n != nullptr; n = n->next) {
            if (!is(n, "DeliveryDestination")) {
                continue;
            }
            DeliveryDest d;
            if (xmlNodePtr addr = child(n, "DeliveryAddress")) {
                for (xmlNodePtr a = addr->children; a != nullptr; a = a->next) {
                    if (a->type != XML_ELEMENT_NODE) {
                        continue;
                    }
                    d.address_kind = C(a->name);
                    d.address = d.address_kind == "IPAddressPort" ? read_ip_port(a) : text_of(a);
                    break;
                }
            }
            d.destination_reference = child_text(n, "DestinationReference");
            d.iri_or_cc = read_entry(child(n, "IRIorCC"));
            d.handover_format = read_entry(child(n, "HandoverFormat"));
            v.destinations.push_back(std::move(d));
        }
    }
    v.associated = object.associated_objects();
    return v;
}

tl::expected<Object, std::string> make_notification(const NotificationParams& p) {
    if (p.identifier.empty()) {
        return tl::make_unexpected(std::string("a NotificationObject needs an identifier"));
    }
    const auto entry_xml = [](const char* element, const char* ns_prefix, const DictionaryEntry& e) {
        return std::string("<") + ns_prefix + ":" + element + "><common:Owner>" + escape(e.owner) +
               "</common:Owner><common:Name>" + escape(e.name) + "</common:Name><common:Value>" +
               escape(e.value) + "</common:Value></" + ns_prefix + ":" + element + ">";
    };
    std::string xml =
        std::string("<HI1Object xmlns=\"") + kCoreNs + "\" xmlns:xsi=\"" + kXsiNs +
        "\" xmlns:common=\"" + kCommonNs + "\" xmlns:notif=\"" + kNotifNs +
        "\" xsi:type=\"notif:NotificationObject\"><ObjectIdentifier>" + escape(p.identifier) +
        "</ObjectIdentifier><CountryCode>" + escape(p.country_code) +
        "</CountryCode><OwnerIdentifier>" + escape(p.owner_identifier) + "</OwnerIdentifier>";
    if (!p.associated.empty()) {
        xml += "<AssociatedObjects>";
        for (const auto& a : p.associated) {
            xml += "<AssociatedObject>" + escape(a) + "</AssociatedObject>";
        }
        xml += "</AssociatedObjects>";
    }
    xml += "<notif:NotificationDetails>" + escape(p.details) + "</notif:NotificationDetails>" +
           entry_xml("NotificationType", "notif", p.type) + "<notif:NewNotification>" +
           (p.new_notification ? "true" : "false") + "</notif:NewNotification>" +
           "<notif:NotificationTimestamp>" + escape(p.timestamp) + "</notif:NotificationTimestamp>";
    if (!p.statuses.empty()) {
        xml += "<notif:StatusOfAssociatedObjects>";
        for (const auto& s : p.statuses) {
            xml += "<notif:AssociatedObjectStatus><notif:AssociatedObject>" + escape(s.object) +
                   "</notif:AssociatedObject>" + entry_xml("Status", "notif", s.status);
            if (s.details) {
                xml += "<notif:Details>" + escape(*s.details) + "</notif:Details>";
            }
            xml += "</notif:AssociatedObjectStatus>";
        }
        xml += "</notif:StatusOfAssociatedObjects>";
    }
    xml += "</HI1Object>";
    return Object::from_xml(std::move(xml));
}

bool is_valid_etsi_version(std::string_view v) {
    // V\d+\.\d+\.\d+ (common:ETSIVersion)
    if (v.size() < 6 || v[0] != 'V') {
        return false;
    }
    int dots = 0;
    bool digit_since_dot = false;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] >= '0' && v[i] <= '9') {
            digit_since_dot = true;
        } else if (v[i] == '.' && digit_since_dot) {
            ++dots;
            digit_since_dot = false;
        } else {
            return false;
        }
    }
    return dots == 2 && digit_since_dot;
}

// ---- parsing ------------------------------------------------------------------------------------

namespace {

Header read_header(xmlNodePtr h) {
    Header out;
    out.sender = read_endpoint(child(h, "SenderIdentifier"));
    out.receiver = read_endpoint(child(h, "ReceiverIdentifier"));
    out.transaction_id = child_text(h, "TransactionIdentifier").value_or("");
    out.timestamp = child_text(h, "Timestamp").value_or("");
    if (xmlNodePtr v = child(h, "Version")) {
        out.version.etsi_version = child_text(v, "ETSIVersion").value_or("");
        out.version.national_profile_owner = child_text(v, "NationalProfileOwner").value_or("");
        out.version.national_profile_version = child_text(v, "NationalProfileVersion").value_or("");
    }
    out.workflow_id = child_text(h, "WorkflowIdentifier");
    return out;
}

// Copy an HI1Object subtree into its own document and return it as a self-contained Object.
// libxml2 re-declares, at the new root, every namespace the copied ELEMENTS and ATTRIBUTES use;
// the prefixes inside xsi:type VALUES are QNames it cannot see, so those are collected first and
// re-declared explicitly.
tl::expected<Object, std::string> extract_object(xmlDocPtr src, xmlNodePtr node) {
    std::vector<std::pair<std::string, std::string>> qname_ns; // prefix -> uri used by xsi:type values
    std::vector<xmlNodePtr> stack{node};
    while (!stack.empty()) {
        xmlNodePtr n = stack.back();
        stack.pop_back();
        if (xmlChar* t = xmlGetNsProp(n, X("type"), X(kXsiNs))) {
            std::string v = C(t);
            xmlFree(t);
            if (const auto colon = v.find(':'); colon != std::string::npos) {
                const std::string prefix = v.substr(0, colon);
                if (xmlNsPtr ns = xmlSearchNs(src, n, X(prefix.c_str()))) {
                    qname_ns.emplace_back(prefix, ns->href != nullptr ? C(ns->href) : "");
                }
            }
        }
        for (xmlNodePtr c = n->children; c != nullptr; c = c->next) {
            if (c->type == XML_ELEMENT_NODE) {
                stack.push_back(c);
            }
        }
    }
    Doc tmp(xmlNewDoc(X("1.0")));
    xmlNodePtr copy = xmlDocCopyNode(node, tmp.d, 1);
    if (copy == nullptr) {
        return tl::make_unexpected(std::string("could not copy the HI1Object"));
    }
    xmlDocSetRootElement(tmp.d, copy);
    xmlReconciliateNs(tmp.d, copy);
    for (const auto& [prefix, uri] : qname_ns) {
        if (xmlSearchNs(tmp.d, copy, X(prefix.c_str())) == nullptr) {
            xmlNewNs(copy, X(uri.c_str()), X(prefix.c_str()));
        }
    }
    return Object::from_xml(dump_node(tmp.d, copy));
}

std::optional<Failure> read_failure(xmlNodePtr n) {
    if (n == nullptr) {
        return std::nullopt;
    }
    Failure f;
    f.code = static_cast<std::uint32_t>(std::strtoul(child_text(n, "ErrorCode").value_or("0").c_str(), nullptr, 10));
    f.description = child_text(n, "ErrorDescription").value_or("");
    return f;
}

} // namespace

tl::expected<Request, ParseError> parse_request(const std::string& xml) {
    Doc doc = parse_xml(xml);
    if (!doc) {
        return tl::make_unexpected(ParseError{ErrorCode::ValidationError, "not well-formed XML", std::nullopt});
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    if (!is(root, "HI1Message")) {
        return tl::make_unexpected(ParseError{ErrorCode::ValidationError, "root is not HI1Message", std::nullopt});
    }
    std::optional<Header> best_effort;
    if (xmlNodePtr h = child(root, "Header")) {
        best_effort = read_header(h);
    }
    std::string why;
    if (!schema_valid(doc.d, &why)) {
        return tl::make_unexpected(ParseError{ErrorCode::ValidationError, why, best_effort});
    }
    xmlNodePtr payload = child(root, "Payload");
    xmlNodePtr req = child(payload, "RequestPayload");
    if (req == nullptr) {
        return tl::make_unexpected(
            ParseError{ErrorCode::ValidationError, "message is not a request", best_effort});
    }
    Request out;
    out.header = *best_effort;
    xmlNodePtr list = child(req, "ActionRequests");
    for (xmlNodePtr a = list->children; a != nullptr; a = a->next) {
        if (!is(a, "ActionRequest")) {
            continue;
        }
        Action action;
        action.id = to_u64(child_text(a, "ActionIdentifier")).value_or(0);
        bool found = false;
        for (xmlNodePtr verb = a->children; verb != nullptr && !found; verb = verb->next) {
            if (verb->type != XML_ELEMENT_NODE || is(verb, "ActionIdentifier")) {
                continue;
            }
            found = true;
            if (is(verb, "GET")) {
                action.body = GetAction{child_text(verb, "Identifier").value_or("")};
            } else if (is(verb, "CREATE") || is(verb, "UPDATE")) {
                auto obj = extract_object(doc.d, child(verb, "HI1Object"));
                if (!obj) {
                    return tl::make_unexpected(
                        ParseError{ErrorCode::ValidationError, obj.error(), best_effort});
                }
                if (is(verb, "CREATE")) {
                    action.body = CreateAction{std::move(*obj)};
                } else {
                    action.body = UpdateAction{std::move(*obj)};
                }
            } else if (is(verb, "LIST")) {
                ListAction l;
                l.object_type = read_entry(child(verb, "ObjectType"));
                l.last_changed = child_text(verb, "LastChanged");
                l.maximum_object_count = to_u64(child_text(verb, "MaximumObjectCount"));
                l.status = read_entry(child(verb, "Status"));
                l.country_code = child_text(verb, "CountryCode");
                action.body = std::move(l);
            } else if (is(verb, "DELIVER")) {
                auto obj = extract_object(doc.d, child(verb, "HI1Object"));
                if (!obj) {
                    return tl::make_unexpected(
                        ParseError{ErrorCode::ValidationError, obj.error(), best_effort});
                }
                action.body = DeliverAction{child_text(verb, "Identifier").value_or(""), std::move(*obj)};
            } else if (is(verb, "GETCSPCONFIG")) {
                action.body = GetCspConfigAction{};
            }
        }
        out.actions.push_back(std::move(action));
    }
    return out;
}

namespace {

ListRecord read_list_record(xmlNodePtr n) {
    ListRecord r;
    r.object_type = read_entry(child(n, "ObjectType")).value_or(DictionaryEntry{});
    r.identifier = child_text(n, "Identifier").value_or("");
    r.country_code = child_text(n, "CountryCode");
    r.owner_identifier = child_text(n, "OwnerIdentifier");
    r.generation = to_u64(child_text(n, "Generation")).value_or(0);
    r.external_identifier = child_text(n, "ExternalIdentifier");
    r.last_changed = child_text(n, "LastChanged");
    return r;
}

CspConfig read_config(xmlNodePtr n) {
    CspConfig c;
    c.last_changed = child_text(n, "LastChanged").value_or("");
    if (xmlNodePtr tf = child(n, "TargetFormatTypeDefinitions")) {
        TargetFormatDefinitions defs;
        defs.format_owner = child_text(tf, "FormatOwner").value_or("");
        if (xmlNodePtr entries = child(tf, "TargetFormatTypeDefinitionEntries")) {
            for (xmlNodePtr e = entries->children; e != nullptr; e = e->next) {
                if (is(e, "TargetFormatTypeDefinitionEntry")) {
                    defs.entries.push_back({child_text(e, "FormatName").value_or(""),
                                            child_text(e, "Description").value_or(""),
                                            child_text(e, "FormatRegex").value_or("")});
                }
            }
        }
        c.target_formats = std::move(defs);
    }
    if (xmlNodePtr tc = child(n, "TargetingConfigurations")) {
        for (xmlNodePtr e = tc->children; e != nullptr; e = e->next) {
            if (!is(e, "TargetingConfiguration")) {
                continue;
            }
            TargetingConfiguration t;
            t.format_name = child_text(e, "FormatName").value_or("");
            t.format_owner = child_text(e, "FormatOwner").value_or("");
            t.guidance = child_text(e, "Guidance");
            if (xmlNodePtr types = child(e, "AssociatedLIServiceTypes")) {
                for (xmlNodePtr d = types->children; d != nullptr; d = d->next) {
                    if (is(d, "DictionaryEntry")) {
                        t.li_service_types.push_back(read_entry(d).value_or(DictionaryEntry{}));
                    }
                }
            }
            c.targeting.push_back(std::move(t));
        }
    }
    if (xmlNodePtr eps = child(n, "SupportedLIWorkflowEndpoints")) {
        for (xmlNodePtr e = eps->children; e != nullptr; e = e->next) {
            if (is(e, "SupportedLIWorkflowEndpoint")) {
                WorkflowEndpoint w;
                w.endpoint = read_entry(child(e, "LIWorkflowEndpoint")).value_or(DictionaryEntry{});
                w.guidance = child_text(e, "Guidance");
                w.url = child_text(e, "URL").value_or("");
                c.li_endpoints.push_back(std::move(w));
            }
        }
    }
    return c;
}

} // namespace

tl::expected<Response, ParseError> parse_response(const std::string& xml) {
    Doc doc = parse_xml(xml);
    if (!doc) {
        return tl::make_unexpected(ParseError{ErrorCode::ValidationError, "not well-formed XML", std::nullopt});
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.d);
    if (!is(root, "HI1Message")) {
        return tl::make_unexpected(ParseError{ErrorCode::ValidationError, "root is not HI1Message", std::nullopt});
    }
    std::optional<Header> best_effort;
    if (xmlNodePtr h = child(root, "Header")) {
        best_effort = read_header(h);
    }
    std::string why;
    if (!schema_valid(doc.d, &why)) {
        return tl::make_unexpected(ParseError{ErrorCode::ValidationError, why, best_effort});
    }
    xmlNodePtr resp = child(child(root, "Payload"), "ResponsePayload");
    if (resp == nullptr) {
        return tl::make_unexpected(
            ParseError{ErrorCode::ValidationError, "message is not a response", best_effort});
    }
    Response out;
    out.header = *best_effort;
    if (xmlNodePtr top = child(resp, "ErrorInformation")) {
        out.payload = *read_failure(top);
        return out;
    }
    std::vector<ActionResult> results;
    for (xmlNodePtr a = child(resp, "ActionResponses")->children; a != nullptr; a = a->next) {
        if (!is(a, "ActionResponse")) {
            continue;
        }
        ActionResult r;
        r.id = to_u64(child_text(a, "ActionIdentifier")).value_or(0);
        for (xmlNodePtr v = a->children; v != nullptr; v = v->next) {
            if (v->type != XML_ELEMENT_NODE || is(v, "ActionIdentifier")) {
                continue;
            }
            if (is(v, "ErrorInformation")) {
                r.outcome = *read_failure(v);
            } else if (is(v, "GETResponse")) {
                auto obj = extract_object(doc.d, child(v, "HI1Object"));
                if (!obj) {
                    return tl::make_unexpected(
                        ParseError{ErrorCode::ValidationError, obj.error(), best_effort});
                }
                r.outcome = GetResult{std::move(*obj)};
            } else if (is(v, "CREATEResponse") || is(v, "UPDATEResponse")) {
                std::optional<Object> obj;
                if (xmlNodePtr on = child(v, "HI1Object")) {
                    auto e = extract_object(doc.d, on);
                    if (!e) {
                        return tl::make_unexpected(
                            ParseError{ErrorCode::ValidationError, e.error(), best_effort});
                    }
                    obj = std::move(*e);
                }
                const std::string id = child_text(v, "Identifier").value_or("");
                if (is(v, "CREATEResponse")) {
                    r.outcome = CreateResult{id, std::move(obj)};
                } else {
                    r.outcome = UpdateResult{id, std::move(obj)};
                }
            } else if (is(v, "LISTResponse")) {
                ListResult lr;
                for (xmlNodePtr rec = v->children; rec != nullptr; rec = rec->next) {
                    if (is(rec, "ListResponseRecord")) {
                        lr.records.push_back(read_list_record(rec));
                    }
                }
                r.outcome = std::move(lr);
            } else if (is(v, "DELIVERResponse")) {
                r.outcome = DeliverResult{child_text(v, "Identifier").value_or("")};
            } else if (is(v, "GETCSPCONFIGResponse")) {
                r.outcome = ConfigResult{read_config(v)};
            }
            break;
        }
        results.push_back(std::move(r));
    }
    out.payload = std::move(results);
    return out;
}

// ---- building -----------------------------------------------------------------------------------

namespace {

struct Builder {
    Doc doc;
    xmlNodePtr root = nullptr;
    xmlNsPtr core = nullptr;
    xmlNsPtr common = nullptr;
    xmlNsPtr config = nullptr;
    xmlNsPtr xsi = nullptr;

    Builder() : doc(xmlNewDoc(X("1.0"))) {
        root = xmlNewDocNode(doc.d, nullptr, X("HI1Message"), nullptr);
        core = xmlNewNs(root, X(kCoreNs), nullptr);
        xmlSetNs(root, core);
        xsi = xmlNewNs(root, X(kXsiNs), X("xsi"));
        common = xmlNewNs(root, X(kCommonNs), X("common"));
        config = xmlNewNs(root, X(kConfigNs), X("config"));
        xmlNewNs(root, X(kAuthNs), X("auth"));
        xmlNewNs(root, X(kTaskNs), X("task"));
        xmlNewNs(root, X(kDocNs), X("doc"));
        xmlNewNs(root, X(kNotifNs), X("notif"));
        xmlDocSetRootElement(doc.d, root);
    }

    xmlNodePtr node(xmlNodePtr parent, xmlNsPtr ns, const char* name) {
        return xmlNewChild(parent, ns, X(name), nullptr);
    }
    xmlNodePtr text(xmlNodePtr parent, xmlNsPtr ns, const char* name, const std::string& v) {
        return xmlNewTextChild(parent, ns, X(name), X(v.c_str()));
    }
    void entry(xmlNodePtr parent, xmlNsPtr elem_ns, const char* name, const DictionaryEntry& e) {
        xmlNodePtr n = node(parent, elem_ns, name);
        text(n, common, "Owner", e.owner);
        text(n, common, "Name", e.name);
        text(n, common, "Value", e.value);
    }
    void endpoint(xmlNodePtr parent, const char* name, const EndpointId& id) {
        xmlNodePtr n = node(parent, core, name);
        text(n, core, "CountryCode", id.country_code);
        text(n, core, "UniqueIdentifier", id.unique_identifier);
    }
    void header(const Header& h) {
        xmlNodePtr n = node(root, core, "Header");
        endpoint(n, "SenderIdentifier", h.sender);
        endpoint(n, "ReceiverIdentifier", h.receiver);
        text(n, core, "TransactionIdentifier", h.transaction_id);
        text(n, core, "Timestamp", h.timestamp);
        xmlNodePtr v = node(n, core, "Version");
        text(v, core, "ETSIVersion", h.version.etsi_version);
        text(v, core, "NationalProfileOwner", h.version.national_profile_owner);
        text(v, core, "NationalProfileVersion", h.version.national_profile_version);
        if (h.workflow_id) {
            text(n, core, "WorkflowIdentifier", *h.workflow_id);
        }
    }
    // Attach a stored object under `parent` (a copy of its self-contained element).
    bool object(xmlNodePtr parent, const Object& o) {
        Doc src = parse_xml(o.xml());
        if (!src) {
            return false;
        }
        xmlNodePtr copy = xmlDocCopyNode(xmlDocGetRootElement(src.d), doc.d, 1);
        if (copy == nullptr) {
            return false;
        }
        xmlAddChild(parent, copy);
        xmlReconciliateNs(doc.d, copy);
        return true;
    }
    void failure(xmlNodePtr parent, const Failure& f) {
        text(parent, core, "ErrorCode", std::to_string(f.code));
        text(parent, core, "ErrorDescription", f.description);
    }
};

tl::expected<std::string, std::string> finish(Builder& b) {
    std::string why;
    if (!schema_valid(b.doc.d, &why)) {
        return tl::make_unexpected("built message failed schema validation: " + why);
    }
    return dump_doc(b.doc.d);
}

void write_config(Builder& b, xmlNodePtr parent, const CspConfig& c) {
    b.text(parent, b.core, "LastChanged", c.last_changed);
    if (c.target_formats) {
        xmlNodePtr tf = b.node(parent, b.core, "TargetFormatTypeDefinitions");
        b.text(tf, b.config, "FormatOwner", c.target_formats->format_owner);
        xmlNodePtr entries = b.node(tf, b.config, "TargetFormatTypeDefinitionEntries");
        for (const auto& e : c.target_formats->entries) {
            xmlNodePtr n = b.node(entries, b.config, "TargetFormatTypeDefinitionEntry");
            b.text(n, b.config, "FormatName", e.format_name);
            b.text(n, b.config, "Description", e.description);
            b.text(n, b.config, "FormatRegex", e.format_regex);
        }
    }
    xmlNodePtr tcs = b.node(parent, b.core, "TargetingConfigurations");
    for (const auto& t : c.targeting) {
        xmlNodePtr n = b.node(tcs, b.config, "TargetingConfiguration");
        b.text(n, b.config, "FormatName", t.format_name);
        b.text(n, b.config, "FormatOwner", t.format_owner);
        if (t.guidance) {
            b.text(n, b.config, "Guidance", *t.guidance);
        }
        if (!t.li_service_types.empty()) {
            xmlNodePtr types = b.node(n, b.config, "AssociatedLIServiceTypes");
            for (const auto& e : t.li_service_types) {
                b.entry(types, b.common, "DictionaryEntry", e);
            }
        }
    }
    if (!c.li_endpoints.empty()) {
        xmlNodePtr eps = b.node(parent, b.core, "SupportedLIWorkflowEndpoints");
        for (const auto& w : c.li_endpoints) {
            xmlNodePtr n = b.node(eps, b.config, "SupportedLIWorkflowEndpoint");
            b.entry(n, b.config, "LIWorkflowEndpoint", w.endpoint);
            if (w.guidance) {
                b.text(n, b.config, "Guidance", *w.guidance);
            }
            b.text(n, b.config, "URL", w.url);
        }
    }
}

} // namespace

tl::expected<std::string, std::string> serialise_request(const Request& request) {
    if (request.actions.empty()) {
        return tl::make_unexpected(std::string("a request carries at least one action (6.3.2)"));
    }
    Builder b;
    b.header(request.header);
    xmlNodePtr payload = b.node(b.root, b.core, "Payload");
    xmlNodePtr req = b.node(payload, b.core, "RequestPayload");
    xmlNodePtr list = b.node(req, b.core, "ActionRequests");
    for (const auto& action : request.actions) {
        xmlNodePtr a = b.node(list, b.core, "ActionRequest");
        b.text(a, b.core, "ActionIdentifier", std::to_string(action.id));
        bool ok = true;
        std::visit(
            [&](const auto& body) {
                using T = std::decay_t<decltype(body)>;
                if constexpr (std::is_same_v<T, GetAction>) {
                    xmlNodePtr v = b.node(a, b.core, "GET");
                    b.text(v, b.core, "Identifier", body.identifier);
                } else if constexpr (std::is_same_v<T, CreateAction>) {
                    ok = b.object(b.node(a, b.core, "CREATE"), body.object);
                } else if constexpr (std::is_same_v<T, UpdateAction>) {
                    ok = b.object(b.node(a, b.core, "UPDATE"), body.object);
                } else if constexpr (std::is_same_v<T, ListAction>) {
                    xmlNodePtr v = b.node(a, b.core, "LIST");
                    if (body.object_type) {
                        b.entry(v, b.core, "ObjectType", *body.object_type);
                    }
                    if (body.last_changed) {
                        b.text(v, b.core, "LastChanged", *body.last_changed);
                    }
                    if (body.maximum_object_count) {
                        b.text(v, b.core, "MaximumObjectCount", std::to_string(*body.maximum_object_count));
                    }
                    if (body.status) {
                        b.entry(v, b.core, "Status", *body.status);
                    }
                    if (body.country_code) {
                        b.text(v, b.core, "CountryCode", *body.country_code);
                    }
                } else if constexpr (std::is_same_v<T, DeliverAction>) {
                    xmlNodePtr v = b.node(a, b.core, "DELIVER");
                    b.text(v, b.core, "Identifier", body.identifier);
                    ok = b.object(v, body.object);
                } else if constexpr (std::is_same_v<T, GetCspConfigAction>) {
                    b.node(a, b.core, "GETCSPCONFIG");
                }
            },
            action.body);
        if (!ok) {
            return tl::make_unexpected(std::string("could not attach an HI1Object to the request"));
        }
    }
    return finish(b);
}

tl::expected<std::string, std::string> serialise_response(const Response& response) {
    Builder b;
    b.header(response.header);
    xmlNodePtr payload = b.node(b.root, b.core, "Payload");
    xmlNodePtr resp = b.node(payload, b.core, "ResponsePayload");
    if (const auto* top = std::get_if<Failure>(&response.payload)) {
        b.failure(b.node(resp, b.core, "ErrorInformation"), *top);
        return finish(b);
    }
    const auto& results = std::get<std::vector<ActionResult>>(response.payload);
    if (results.empty()) {
        return tl::make_unexpected(std::string("a response carries at least one action response"));
    }
    xmlNodePtr list = b.node(resp, b.core, "ActionResponses");
    for (const auto& r : results) {
        xmlNodePtr a = b.node(list, b.core, "ActionResponse");
        b.text(a, b.core, "ActionIdentifier", std::to_string(r.id));
        bool ok = true;
        std::visit(
            [&](const auto& out) {
                using T = std::decay_t<decltype(out)>;
                if constexpr (std::is_same_v<T, GetResult>) {
                    ok = b.object(b.node(a, b.core, "GETResponse"), out.object);
                } else if constexpr (std::is_same_v<T, CreateResult> || std::is_same_v<T, UpdateResult>) {
                    xmlNodePtr v =
                        b.node(a, b.core, std::is_same_v<T, CreateResult> ? "CREATEResponse" : "UPDATEResponse");
                    b.text(v, b.core, "Identifier", out.identifier);
                    if (out.object) {
                        ok = b.object(v, *out.object);
                    }
                } else if constexpr (std::is_same_v<T, ListResult>) {
                    xmlNodePtr v = b.node(a, b.core, "LISTResponse");
                    for (const auto& rec : out.records) {
                        xmlNodePtr n = b.node(v, b.core, "ListResponseRecord");
                        b.entry(n, b.core, "ObjectType", rec.object_type);
                        b.text(n, b.core, "Identifier", rec.identifier);
                        if (rec.country_code) {
                            b.text(n, b.core, "CountryCode", *rec.country_code);
                        }
                        if (rec.owner_identifier) {
                            b.text(n, b.core, "OwnerIdentifier", *rec.owner_identifier);
                        }
                        b.text(n, b.core, "Generation", std::to_string(rec.generation));
                        if (rec.external_identifier) {
                            b.text(n, b.core, "ExternalIdentifier", *rec.external_identifier);
                        }
                        if (rec.last_changed) {
                            b.text(n, b.core, "LastChanged", *rec.last_changed);
                        }
                    }
                } else if constexpr (std::is_same_v<T, DeliverResult>) {
                    b.text(b.node(a, b.core, "DELIVERResponse"), b.core, "Identifier", out.identifier);
                } else if constexpr (std::is_same_v<T, ConfigResult>) {
                    write_config(b, b.node(a, b.core, "GETCSPCONFIGResponse"), out.config);
                } else if constexpr (std::is_same_v<T, Failure>) {
                    b.failure(b.node(a, b.core, "ErrorInformation"), out);
                }
            },
            r.outcome);
        if (!ok) {
            return tl::make_unexpected(std::string("could not attach an HI1Object to the response"));
        }
    }
    return finish(b);
}

} // namespace li_core::hi1
