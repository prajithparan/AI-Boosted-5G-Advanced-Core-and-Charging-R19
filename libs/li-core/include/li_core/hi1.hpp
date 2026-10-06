#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tl/expected.hpp>
#include <variant>
#include <vector>

// ETSI TS 103 120 LI_HI1 message codec, XML encoding only (ADR-0462 step 2). TS 103 120 V1.24.1
// is the prose read; the schemas in specs/etsi/103120 are v1.23.1 (the shapes authority). The
// transport (clause 9.3: HTTPS POST, always HTTP 200, errors in the body) lives in the ADMF
// process, not here.
//
// Design: an HI1 object is held as its own self-contained XML element (`Object`), not re-modelled
// field by field. An HI1Object carries 15-20 optional members, several of them deep and
// nationally extensible (ApprovalDetails, NationalXxxParameters, policy references); modelling
// each would silently drop whatever it missed, and a warrant record must come back from GET exactly
// as the LEA created it. `Object` instead gives typed READS of the members the ADMF's logic uses
// and ordered EDITS (a replaced or inserted member lands at its XSD-sequence position), and every
// message the codec emits is validated against the schema first (clause 9.2.1: "Sender and
// Receiver shall only send messages that are successfully validated"). libxml2 stays PRIVATE to
// li_core; no type here names it.
//
// Not implemented, disclosed: XML signatures (clause 9.2.3 -- hi1-validation.xsd wires xmldsig to a
// stub, so a signed message is accepted and its signature NOT verified), the JSON encoding, the
// Dictionaries element of a GETCSPCONFIG response, and the LD/LP/TD/TrafficPolicy/IRIPolicy object
// kinds (they parse as ObjectType::Other and round-trip, but have no typed view).

#pragma GCC visibility push(default)
namespace li_core::hi1 {

// TS 103 120 Annex D error codes used by this codec (table D.1).
enum class ErrorCode : std::uint32_t {
    GeneralBusinessLogicError = 3000,
    FeatureNotSupported = 3001,
    DuplicateActionId = 3002,
    TransientTechnicalError = 3003,
    ConfigurationIssue = 3004,
    RequiredElementMissing = 3005,
    ValueChangeNotAllowed = 3006,
    ImproperValue = 3007,
    ImproperValueChange = 3008,
    ValueNotFound = 3009,
    ObjectAlreadyExists = 3010,
    UpdateObjectDoesNotExist = 3011,
    UpdateObjectExpired = 3012,
    CancelObjectDoesNotExist = 3013,
    GetObjectNotFound = 3014,
    GetObjectNotDeliverable = 3015,
    LinkTargetDoesNotExist = 3016,
    LinkTargetExpired = 3017,
    LinkTargetFailed = 3018,
    UnsupportedEncoding = 3019,
    ValidationError = 3020,
    VersionNotSupported = 3021,
    MessageTooLarge = 3022,
};

struct DictionaryEntry { // common:DictionaryEntry: Owner, Name, Value
    std::string owner;
    std::string name;
    std::string value;
    bool operator==(const DictionaryEntry&) const = default;
};

struct EndpointId {
    std::string country_code; // ISO 3166-1 alpha-2 (or ETSI's "XX" test code)
    std::string unique_identifier;
    bool operator==(const EndpointId&) const = default;
};

struct Version {
    std::string etsi_version; // "V1.23.1"
    std::string national_profile_owner;
    std::string national_profile_version;
    bool operator==(const Version&) const = default;
};

// Clause 6.2 MessageHeader.
struct Header {
    EndpointId sender;
    EndpointId receiver;
    std::string transaction_id; // UUID
    std::string timestamp;      // QualifiedMicrosecondDateTime
    Version version;
    std::optional<std::string> workflow_id;
    bool operator==(const Header&) const = default;
};

enum class ObjectType : std::uint8_t { Authorisation, LITask, Document, Notification, Other };

// One HI1Object, as its own self-contained XML element (see the header comment).
class Object {
public:
    // Parse and structurally check an HI1Object element (well-formed, has an ObjectIdentifier and
    // an xsi:type). It is NOT schema-validated alone -- the schema validates whole messages.
    static tl::expected<Object, std::string> from_xml(std::string xml);

    [[nodiscard]] const std::string& xml() const { return xml_; }
    [[nodiscard]] ObjectType type() const { return type_; }
    // Local name of the xsi:type, e.g. "AuthorisationObject".
    [[nodiscard]] const std::string& type_name() const { return type_name_; }
    [[nodiscard]] const std::string& identifier() const { return identifier_; }

    // Typed reads of direct children, by local name. nullopt if absent.
    [[nodiscard]] std::optional<std::string> text(std::string_view field) const;
    [[nodiscard]] std::optional<DictionaryEntry> entry(std::string_view field) const;
    [[nodiscard]] std::vector<std::string> associated_objects() const;
    // The text of a nested member, by local-name path from the object root, e.g.
    // {"DocumentBody", "ContentType"}. nullopt if any step is absent.
    [[nodiscard]] std::optional<std::string> text_at(const std::vector<std::string>& path) const;

    // Ordered edits. `field` must be a member of this object's type (base HI1Object members or
    // the type's own) -- anything else is an error, never a guess. A new member is inserted at its
    // XSD-sequence position; an existing one is replaced in place.
    tl::expected<void, std::string> set_text(std::string_view field, const std::string& value);
    tl::expected<void, std::string> set_entry(std::string_view field, const DictionaryEntry& entry);
    tl::expected<void, std::string> set_associated_objects(const std::vector<std::string>& ids);
    bool remove(std::string_view field);

    // Set an ActionUnsuccesfulInformation member (AuthorisationInvalidReason / InvalidReason /
    // DocumentInvalidReason): ErrorCode + ErrorDescription, at its schema position.
    tl::expected<void, std::string>
    set_failure(std::string_view field, std::uint32_t code, const std::string& description);

    // The local names of the members present (ObjectIdentifier included), in document order.
    [[nodiscard]] std::vector<std::string> members() const;

    // TS 103 120 6.4.7 UPDATE semantics: every member present in `update` replaces the stored one
    // wholesale (a list member is overwritten, never appended to; an empty list clears it); a
    // member absent from `update` is left unchanged. ObjectIdentifier is never changed. The two
    // objects must be of the same type, else an error.
    tl::expected<void, std::string> merge(const Object& update);

private:
    std::string xml_;
    ObjectType type_ = ObjectType::Other;
    std::string type_name_;
    std::string identifier_;
};

// ---- typed views of the objects the LI lifecycle workflow profile (Annex H.5) uses --------------

struct AuthorisationView {
    std::optional<std::string> reference;
    std::optional<DictionaryEntry> status;
    std::optional<DictionaryEntry> desired_status;
    std::optional<std::string> start_time;
    std::optional<std::string> end_time;
    std::vector<EndpointId> cspids;
};
AuthorisationView view_authorisation(const Object& object);

struct TargetValue {
    std::string format_owner;
    std::string format_name; // e.g. "SUPIIMSI", "IMSI", "InternationalE164" (Annex C)
    std::string value;
    bool operator==(const TargetValue&) const = default;
};
struct DeliveryDest {
    std::string address_kind; // the DeliveryAddress choice element, e.g. "IPAddressPort"
    std::string address;      // "ip:port" for IPAddressPort, else the element text
    std::optional<std::string> destination_reference;
    std::optional<DictionaryEntry> iri_or_cc;
    std::optional<DictionaryEntry> handover_format;
};
struct LiTaskView {
    std::optional<std::string> liid; // the task's Reference
    std::optional<DictionaryEntry> status;
    std::optional<DictionaryEntry> desired_status;
    std::optional<std::string> start_time;
    std::optional<std::string> end_time;
    std::vector<TargetValue> targets;
    std::optional<DictionaryEntry> service_type;
    std::optional<DictionaryEntry> delivery_type; // TaskDeliveryType: IRIOnly / CCOnly / IRIandCC
    std::vector<DeliveryDest> destinations;
    std::vector<std::string> associated; // the Authorisation(s) it belongs to
};
LiTaskView view_litask(const Object& object);

// Build a NotificationObject (clause 7.4); only the Receiver (CSP) creates these.
struct AssociatedStatus {
    std::string object; // ObjectIdentifier of the associated object
    DictionaryEntry status;
    std::optional<std::string> details;
};
struct NotificationParams {
    std::string identifier;
    std::string country_code;
    std::string owner_identifier;
    std::string details;  // NotificationDetails (human readable)
    DictionaryEntry type; // NotificationType, e.g. ETSI/NotificationType/General
    bool new_notification = true;
    std::string timestamp; // QualifiedDateTime
    std::vector<std::string> associated;
    std::vector<AssociatedStatus> statuses;
};
tl::expected<Object, std::string> make_notification(const NotificationParams& params);

// ---- messages (clause 6) -----------------------------------------------------------------------

struct GetAction {
    std::string identifier;
};
struct CreateAction {
    Object object;
};
struct UpdateAction {
    Object object;
};
struct ListAction {
    std::optional<DictionaryEntry> object_type;
    std::optional<std::string> last_changed;
    std::optional<std::uint64_t> maximum_object_count;
    std::optional<DictionaryEntry> status;
    std::optional<std::string> country_code;
};
struct DeliverAction {
    std::string identifier;
    Object object;
};
struct GetCspConfigAction {};
using ActionBody = std::
    variant<GetAction, CreateAction, UpdateAction, ListAction, DeliverAction, GetCspConfigAction>;
struct Action {
    std::uint64_t id = 0; // ActionIdentifier
    ActionBody body;
};
struct Request {
    Header header;
    std::vector<Action> actions;
};

struct Failure { // ActionUnsuccesfulInformation
    std::uint32_t code = 0;
    std::string description;
    bool operator==(const Failure&) const = default;
};

struct ListRecord {
    DictionaryEntry object_type;
    std::string identifier;
    std::optional<std::string> country_code;
    std::optional<std::string> owner_identifier;
    std::uint64_t generation = 0;
    std::optional<std::string> external_identifier;
    std::optional<std::string> last_changed;
};

// Clause 6.4.11 GETCSPCONFIG response, the members this ADMF publishes.
struct TargetFormatDefinition {
    std::string format_name;
    std::string description;
    std::string format_regex;
};
struct TargetFormatDefinitions {
    std::string format_owner;
    std::vector<TargetFormatDefinition> entries;
};
struct TargetingConfiguration {
    std::string format_name;
    std::string format_owner;
    std::optional<std::string> guidance;
    std::vector<DictionaryEntry> li_service_types;
};
struct WorkflowEndpoint {
    DictionaryEntry endpoint; // ETSI / LIWorkflowEndpoint / NewAuthorisation ...
    std::optional<std::string> guidance;
    std::string url;
};
struct CspConfig {
    std::string last_changed; // QualifiedDateTime
    std::optional<TargetFormatDefinitions> target_formats;
    std::vector<TargetingConfiguration> targeting;
    std::vector<WorkflowEndpoint> li_endpoints;
};

struct GetResult {
    Object object;
};
struct CreateResult {
    std::string identifier;
    std::optional<Object> object;
};
struct UpdateResult {
    std::string identifier;
    std::optional<Object> object;
};
struct ListResult {
    std::vector<ListRecord> records;
};
struct DeliverResult {
    std::string identifier;
};
struct ConfigResult {
    CspConfig config;
};
using ActionOutcome = std::variant<GetResult,
                                   CreateResult,
                                   UpdateResult,
                                   ListResult,
                                   DeliverResult,
                                   ConfigResult,
                                   Failure>;
struct ActionResult {
    std::uint64_t id = 0;
    ActionOutcome outcome;
};
struct Response {
    Header header;
    // Per-action results, or (clause 9.2.2) a top-level failure for a message that could not be
    // processed at all.
    std::variant<std::vector<ActionResult>, Failure> payload;
};

struct ParseError {
    ErrorCode code = ErrorCode::ValidationError;
    std::string detail;
    std::optional<Header> header; // best effort, for the top-level error response
};

// Parse and schema-validate an HI1 request / response message (XML). Hardened like the X1 parser:
// no network access, no entity substitution.
tl::expected<Request, ParseError> parse_request(const std::string& xml);
tl::expected<Response, ParseError> parse_response(const std::string& xml);

// Build a message. The result is validated against the schema before it is returned (9.2.1); a
// failure is returned as an error string, never emitted.
tl::expected<std::string, std::string> serialise_request(const Request& request);
tl::expected<std::string, std::string> serialise_response(const Response& response);

// Whether `version` ("V1.23.1") matches the pattern the schema requires.
bool is_valid_etsi_version(std::string_view version);

} // namespace li_core::hi1
#pragma GCC visibility pop
