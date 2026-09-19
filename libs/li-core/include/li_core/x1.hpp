#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tl/expected.hpp>
#include <variant>
#include <vector>

// ETSI TS 103 221-1 V1.23.1 LI_X1 message codec (ADR-0372), the NE-side subset: parse the X1
// requests an ADMF sends to a Network Element that hosts a POI/TF (ActivateTask, ModifyTask,
// DeactivateTask, DeactivateAllTasks, GetTaskDetails, CreateDestination, RemoveDestination,
// RemoveAllDestinations, Ping, Keepalive) and build the matching responses, all as the
// normative XML of clause 7.2.1. The authoritative schema is
// specs/etsi/103221-1/TS_103_221_01.xsd; this header exposes C++ structs, never libxml2 (which
// is PRIVATE to li_core), and generated XML is validated against the schema
// (specs/etsi/103221-1/x1-validation.xsd wires the imports) -- 7.2.1 requires validating what
// you generate.
//
// Not decoded here (the ADMF/MDF-facing or object-management messages of a later increment):
// CreateObject/ModifyObject/..., GetDestinationDetails, GetAllDetails, the Report* the NE
// SENDS (built by x1_server's keepalive machine, not parsed). An unrecognised or unsupported
// request type parses into RequestMessageType and an Unsupported body, so the server answers
// error 1080; a document that is not schema-valid XML is a TopLevelError (6.1).

#pragma GCC visibility push(default)
namespace li_core::x1 {

// The X1 error codes this codec names (TS 103 221-1 table 6.7-3). Not exhaustive -- the ones the
// NE side raises.
enum class ErrorCode : int {
    GenericError = 1000,
    SyntaxSchemaError = 1010,
    UnsupportedVersion = 1020,
    UnexpectedAdmfIdentifier = 1040,
    UnexpectedNeIdentifier = 1060,
    KeepaliveNotSupported = 1070,
    UnsupportedRequest = 1080,
    XidAlreadyExists = 2010,
    XidDoesNotExist = 2020,
    DidAlreadyExists = 2030,
    DidDoesNotExist = 2040,
    ActivateTaskFailure = 3000,
    ModifyTaskFailure = 3001,
    UnsupportedTargetIdentifier = 3010,
    InvalidDeliveryTypeAndDestinations = 3040,
    DeactivateTaskFailure = 4000,
    DeactivateAllTasksFailure = 5000,
    DeactivateAllTasksNotEnabled = 5010,
    CreateDestinationFailure = 6000,
    UnsupportedDeliveryAddress = 6020,
    RemoveDestinationFailure = 7000,
    DestinationInUse = 7010,
};

// TS 103 221-1 table 6.2.1.2-2 TargetIdentifier formats. The 5G identifiers plus IP; every other
// choice element the schema allows is carried as Kind::Other with `element` naming it, so a task
// with an identifier this increment does not model is preserved rather than dropped -- the NE
// decides in its handler whether it can target on it (error 3010 if not).
enum class TargetIdentifierKind : std::uint8_t {
    SupiImsi,
    SupiNai,
    Suci,
    PeiImei,
    PeiImeisv,
    GpsiMsisdn,
    GpsiNai,
    Imsi,
    Imei,
    Msisdn, // e164Number
    Ipv4Address,
    Ipv6Address,
    Nai,
    Other,
};

struct TargetIdentifier {
    TargetIdentifierKind kind = TargetIdentifierKind::Other;
    std::string element; // the XSD choice element name, always set
    std::string value;
};

// Parse the inner XML of a TS 103 221-2 Matched/Other Target Identifier conditional attribute
// (clause 5.3.18/5.3.19: "the contents of the TargetIdentifier tag without the enclosing
// TargetIdentifier tag itself, encoded in UTF-8"), e.g. "<imsi>204081234567890</imsi>", into the
// same struct a TaskDetails carries. The fragment is NOT schema-validated -- it is not a document
// the X1 schema describes -- so this is the element-name lookup of table 6.2.1.2-2 with the same
// hardened parser settings as parse_request (XML_PARSE_NONET, NOENT deliberately unset). An
// element the table does not name parses as Kind::Other with `element` set, never dropped.
tl::expected<TargetIdentifier, std::string> parse_target_identifier_fragment(std::string_view xml);

enum class DeliveryType : std::uint8_t { X2Only, X3Only, X2AndX3 };

// TS 103 221-1 Annex C.2.2 MediationDetails: the part of a TaskDetails that is meaningful only to
// an MDF. This is where the LIID reaches the MDF -- the ADMF "shall provide the XID to LIID(s)
// mapping to the MDF" (clause 5.1.2), and a task may carry several, each delivered separately.
// `productID` is NOT the LIID: the schema types it as a UUIDv4 and the table calls it optional.
enum class MediationDeliveryType : std::uint8_t { Hi2Only, Hi3Only, Hi2AndHi3 };

struct MediationDetails {
    std::string liid; // TS 103 280 LIID
    MediationDeliveryType delivery = MediationDeliveryType::Hi2AndHi3;
    std::optional<std::string> start_time; // QualifiedMicrosecondDateTime, verbatim
    std::optional<std::string> end_time;
    // C.2.2: "Shall be included if deviation from the taskDetails ListofDIDs is necessary. If
    // included, the details shall be used instead of any delivery destinations specified in the
    // ListOfDIDs field in the TaskDetails structure."
    std::vector<std::string> dids;
};

// TS 103 221-1 6.2.1.2 TaskDetails, the members the NE side reads (the optional
// mediation/policy/service lists are preserved verbatim as raw XML fragments in `extra` so a
// ModifyTask round-trips what it was given without this codec having to model every branch).
struct TaskDetails {
    std::string xid;
    std::vector<TargetIdentifier> targets;
    DeliveryType delivery = DeliveryType::X2AndX3;
    std::vector<std::string> dids;         // Destination IDs
    std::vector<std::string> dsids;        // Destination Set IDs
    std::optional<std::string> product_id; // UUIDv4 when present; NOT the LIID
    // Annex C.2.2, populated when the ADMF provisions an MDF. Empty for a plain NE task.
    std::vector<MediationDetails> mediation_details;
    std::optional<std::uint64_t> correlation_id;
    std::optional<bool> implicit_deactivation_allowed;
};

// TS 103 221-1 6.3.1.2 DeliveryAddress oneOf.
struct DeliveryAddress {
    enum class Kind : std::uint8_t { IpAddressAndPort, E164Number, Uri, EmailAddress } kind{};
    std::string value; // "ip:port" for IpAddressAndPort
};

struct DestinationDetails {
    std::string did;
    std::optional<std::string> friendly_name;
    DeliveryType delivery = DeliveryType::X2AndX3;
    DeliveryAddress address;
};

// The request message type of the RequestMessageType enum this codec handles or names.
enum class MessageType : std::uint8_t {
    ActivateTask,
    ModifyTask,
    DeactivateTask,
    DeactivateAllTasks,
    GetTaskDetails,
    CreateDestination,
    RemoveDestination,
    RemoveAllDestinations,
    Ping,
    Keepalive,
    Unsupported,
};
const char* message_type_name(MessageType t); // the RequestMessageType string

// Per-type request bodies.
struct ActivateTask {
    TaskDetails task;
};
struct ModifyTask {
    TaskDetails task;
};
struct DeactivateTask {
    std::string xid;
};
struct DeactivateAllTasks {};
struct GetTaskDetails {
    std::string xid;
};
struct CreateDestination {
    DestinationDetails destination;
};
struct RemoveDestination {
    std::string did;
};
struct RemoveAllDestinations {};
struct Ping {};
struct Keepalive {};
struct Unsupported {
    std::string request_message_type;
}; // the raw xsi:type, for error 1080

using RequestBody = std::variant<ActivateTask,
                                 ModifyTask,
                                 DeactivateTask,
                                 DeactivateAllTasks,
                                 GetTaskDetails,
                                 CreateDestination,
                                 RemoveDestination,
                                 RemoveAllDestinations,
                                 Ping,
                                 Keepalive,
                                 Unsupported>;

// The common envelope of every X1 message (table 6.1-1).
struct MessageHeader {
    std::string admf_identifier;
    std::string ne_identifier;
    std::string message_timestamp; // TS 103 280 QualifiedMicrosecondDateTime
    std::string version;           // "v1.x.y"
    std::string x1_transaction_id; // UUIDv4
};

struct Request {
    MessageHeader header;
    MessageType type = MessageType::Unsupported;
    RequestBody body;
};

// A RequestContainer holds one or more requests, all from the same requester (6.1).
struct RequestContainer {
    std::vector<Request> requests;
};

// Parsing outcome. A document that is not well-formed / not schema-valid is a TopLevelError
// (6.1): the response carries only the envelope and a TopLevelError flag, so `header` is filled
// best-effort (identifiers pulled from the doc if present) and `requests` is empty.
struct ParseError {
    bool top_level = false; // true -> respond with X1TopLevelErrorResponse
    std::string detail;
    std::optional<MessageHeader> header; // best-effort, for the TopLevelError response
};

// Parse an X1Request document. Validates against the schema first (7.2.1).
tl::expected<RequestContainer, ParseError> parse_request(const std::string& xml);

// One response to one request.
struct OkResponse {
    MessageHeader header;
    MessageType type = MessageType::Ping;
    bool acknowledged_and_completed = true; // OK: "AcknowledgedAndCompleted" vs "Acknowledged"
};
struct ErrorResponse {
    MessageHeader header;
    MessageType type = MessageType::Unsupported;
    ErrorCode code = ErrorCode::GenericError;
    std::string description;
};

// GetTaskDetails is parsed (a POI can log the query) but its rich response -- full TaskDetails
// plus a TaskStatus with provisioningStatus and listOfFaults (6.4.2.2) -- is not built in this
// increment; the NE answers it with error 1080, which is a conformant response for a query an NE
// does not support. The task-status response lands with the ADMF/admin increment.
using ResponseItem = std::variant<OkResponse, ErrorResponse>;

// Build an X1Response (ResponseContainer) from the response items. Validates the result against
// the schema before returning it (7.2.1); a schema failure is a bug in this codec, returned as
// an error string rather than emitted.
tl::expected<std::string, std::string> serialise_response(const std::vector<ResponseItem>& items);

// Build the X1TopLevelErrorResponse for an unparseable request (6.1).
std::string serialise_top_level_error(const MessageHeader& header);

// The schema path (the import-wiring wrapper), resolved from the build-time LI_ETSI_SCHEMA_DIR.
const char* schema_path();

} // namespace li_core::x1
#pragma GCC visibility pop
