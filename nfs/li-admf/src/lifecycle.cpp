#include "lifecycle.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>

#include "hi1_service.hpp"

namespace li_admf {

namespace hi1 = li_core::hi1;
using hi1::ErrorCode;
using hi1::ObjectType;

namespace {

// ---- paths
// ---------------------------------------------------------------------------------------

struct WorkflowPath {
    const char* path;
    Workflow workflow;
};
constexpr WorkflowPath kPaths[] = {
    {"/", Workflow::None},
    {"/li/authorisation/new", Workflow::NewAuthorisation},
    {"/li/authorisation/extension", Workflow::AuthorisationExtension},
    {"/li/authorisation/cancellation", Workflow::AuthorisationCancellation},
    {"/li/task/addition", Workflow::TaskAddition},
    {"/li/task/cancellation", Workflow::TaskCancellation},
    {"/li/task/change-delivery", Workflow::ChangeOfDelivery},
};

// ---- small helpers
// -------------------------------------------------------------------------------

std::string lea_key(const hi1::EndpointId& e) {
    return e.country_code + "/" + e.unique_identifier;
}

const char* type_text(ObjectType t) {
    switch (t) {
        case ObjectType::Authorisation:
            return "Authorisation";
        case ObjectType::LITask:
            return "LITask";
        case ObjectType::Document:
            return "Document";
        case ObjectType::Notification:
            return "Notification";
        case ObjectType::Other:
            break;
    }
    return "Other";
}

// The status / desired-status / invalid-reason member names per object type (XSD names).
const char* status_member(ObjectType t) {
    return t == ObjectType::Authorisation ? "AuthorisationStatus"
           : t == ObjectType::Document    ? "DocumentStatus"
                                          : "Status";
}
const char* desired_member(ObjectType t) {
    return t == ObjectType::Authorisation ? "AuthorisationDesiredStatus"
           : t == ObjectType::Document    ? "DocumentDesiredStatus"
                                          : "DesiredStatus";
}
const char* invalid_member(ObjectType t) {
    return t == ObjectType::Authorisation ? "AuthorisationInvalidReason"
           : t == ObjectType::Document    ? "DocumentInvalidReason"
                                          : "InvalidReason";
}
// The ETSI dictionary the status member draws from (tables 7.6, 8.2, document status).
hi1::DictionaryEntry status_entry(ObjectType t, const std::string& value) {
    const char* name = t == ObjectType::Authorisation ? "AuthorisationStatus"
                       : t == ObjectType::Document    ? "DocumentStatus"
                                                      : "TaskStatus";
    return {"ETSI", name, value};
}

bool terminal(const std::string& status) {
    return status == "Cancelled" || status == "Rejected" || status == "Expired";
}

// "YYYY-MM-DDTHH:MM:SS(.ffffff)?(Z|+hh:mm|-hh:mm)" -> epoch seconds.
std::optional<std::time_t> parse_time(const std::string& s) {
    if (s.size() < 20) {
        return std::nullopt;
    }
    std::tm tm{};
    if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == nullptr) {
        return std::nullopt;
    }
    std::time_t t = timegm(&tm);
    // skip fractional seconds
    std::size_t i = 19;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
    }
    if (i < s.size() && (s[i] == '+' || s[i] == '-') && s.size() >= i + 6) {
        const int hh = std::atoi(s.substr(i + 1, 2).c_str());
        const int mm = std::atoi(s.substr(i + 4, 2).c_str());
        const int offset = (hh * 60 + mm) * 60;
        t += s[i] == '+' ? -offset : offset;
    }
    return t;
}

// QualifiedDateTime -> QualifiedMicrosecondDateTime (what X1 wants): ".000000" before the zone.
std::string to_microsecond(const std::string& s) {
    std::size_t i = 19;
    if (s.size() <= i || s[i] == '.') {
        return s;
    }
    return s.substr(0, i) + ".000000" + s.substr(i);
}

// Copy projections from an Object into the stored row.
void project(StoredObject& s, const hi1::Object& obj, ObjectType type) {
    s.object_id = obj.identifier();
    s.object_type = type_text(type);
    s.owner_identifier = obj.text("OwnerIdentifier").value_or("");
    s.country_code = obj.text("CountryCode").value_or("");
    s.generation = std::strtoull(obj.text("Generation").value_or("0").c_str(), nullptr, 10);
    s.external_id = obj.text("ExternalIdentifier").value_or("");
    s.status = obj.entry(status_member(type)).value_or(hi1::DictionaryEntry{}).value;
    s.xml = obj.xml();
}

hi1::ActionResult failure(std::uint64_t id, ErrorCode code, const std::string& why) {
    return {id, hi1::Failure{static_cast<std::uint32_t>(code), why}};
}

hi1::Failure top_failure(ErrorCode code, const std::string& why) {
    return {static_cast<std::uint32_t>(code), why};
}

// ---- workflow shape (message contents of H.5.3 - H.5.8)
// ------------------------------------------

struct Counts {
    int create_auth = 0, create_task = 0, create_doc = 0;
    int update_auth = 0, update_task = 0, update_doc = 0;
    int other = 0; // GET / LIST / DELIVER / GETCSPCONFIG / unsupported object types
};

Counts count(const std::vector<hi1::Action>& actions) {
    Counts c;
    for (const auto& a : actions) {
        if (const auto* cr = std::get_if<hi1::CreateAction>(&a.body)) {
            switch (cr->object.type()) {
                case ObjectType::Authorisation:
                    ++c.create_auth;
                    break;
                case ObjectType::LITask:
                    ++c.create_task;
                    break;
                case ObjectType::Document:
                    ++c.create_doc;
                    break;
                default:
                    ++c.other;
                    break;
            }
        } else if (const auto* up = std::get_if<hi1::UpdateAction>(&a.body)) {
            switch (up->object.type()) {
                case ObjectType::Authorisation:
                    ++c.update_auth;
                    break;
                case ObjectType::LITask:
                    ++c.update_task;
                    break;
                case ObjectType::Document:
                    ++c.update_doc;
                    break;
                default:
                    ++c.other;
                    break;
            }
        } else {
            ++c.other;
        }
    }
    return c;
}

std::optional<hi1::Failure> check_shape(Workflow wf, const std::vector<hi1::Action>& actions) {
    const Counts c = count(actions);
    const auto bad = [](ErrorCode code, const char* why) {
        return std::optional(top_failure(code, why));
    };
    if (wf == Workflow::None) {
        return std::nullopt;
    }
    if (c.other != 0) {
        return bad(ErrorCode::ImproperValue,
                   "this workflow endpoint takes only CREATE/UPDATE of Authorisation, LITask and "
                   "Document objects");
    }
    switch (wf) {
        case Workflow::NewAuthorisation:
            if (c.update_auth + c.update_task + c.update_doc != 0) {
                return bad(ErrorCode::ImproperValue,
                           "New Authorisation takes only CREATE requests (H.5.3.3)");
            }
            if (c.create_auth != 1 || c.create_task < 1 || c.create_doc < 1) {
                return bad(ErrorCode::RequiredElementMissing,
                           "New Authorisation needs exactly one Authorisation, at least one LITask "
                           "and at least one Document CREATE (H.5.3.3)");
            }
            break;
        case Workflow::AuthorisationExtension:
            if (c.update_auth != 1 || c.create_doc < 1 ||
                c.create_auth + c.create_task + c.update_doc != 0) {
                return bad(ErrorCode::RequiredElementMissing,
                           "Authorisation Extension needs one UPDATE of the Authorisation, any "
                           "UPDATEs of its LITasks and at least one Document CREATE (H.5.4.3)");
            }
            break;
        case Workflow::AuthorisationCancellation:
            if (c.update_auth != 1 ||
                c.create_auth + c.create_task + c.update_task + c.update_doc != 0) {
                return bad(ErrorCode::RequiredElementMissing,
                           "Authorisation Cancellation needs one UPDATE of the Authorisation and "
                           "any Document CREATEs (H.5.5.3)");
            }
            break;
        case Workflow::TaskAddition:
            if (c.create_task < 1 || c.create_doc < 1 ||
                c.create_auth + c.update_auth + c.update_task + c.update_doc != 0) {
                return bad(ErrorCode::RequiredElementMissing,
                           "Task Addition needs at least one LITask CREATE and at least one "
                           "Document CREATE (H.5.6.3)");
            }
            break;
        case Workflow::TaskCancellation:
            if (c.update_task < 1 ||
                c.create_auth + c.create_task + c.update_auth + c.update_doc != 0) {
                return bad(ErrorCode::RequiredElementMissing,
                           "Task Cancellation needs at least one LITask UPDATE and any Document "
                           "CREATEs (H.5.7.3)");
            }
            break;
        case Workflow::ChangeOfDelivery:
            if (c.update_task < 1 ||
                c.create_auth + c.create_task + c.create_doc + c.update_auth != 0) {
                return bad(ErrorCode::RequiredElementMissing,
                           "Change of Delivery needs at least one LITask UPDATE and any Document "
                           "UPDATEs (H.5.8.3)");
            }
            break;
        case Workflow::None:
            break;
    }
    return std::nullopt;
}

} // namespace

std::optional<Workflow> workflow_for_path(std::string_view path) {
    for (const auto& p : kPaths) {
        if (path == p.path) {
            return p.workflow;
        }
    }
    return std::nullopt;
}

// =================================================================================================

struct Lifecycle::Impl {
    Impl(LifecycleConfig c, Hi1Store& s, Lipf& l) : config(std::move(c)), store(s), lipf(l) {}

    LifecycleConfig config;
    Hi1Store& store;
    Lipf& lipf;

    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool woken = false;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex reconcile_mutex; // one pass at a time (worker and tests)

    // -------------------------------------------------------------------------------------------
    // request phase
    // -------------------------------------------------------------------------------------------

    bool acceptable_content_type(const std::string& ct) const {
        static const std::set<std::string> kBase = {
            "application/pdf",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            "image/png",
            "image/jpeg",
            "text/plain"};
        return kBase.count(ct) != 0 || std::find(config.extra_document_content_types.begin(),
                                                 config.extra_document_content_types.end(),
                                                 ct) != config.extra_document_content_types.end();
    }

    // What this message creates, by id, so a link to an object in the same message resolves.
    struct MessageObject {
        ObjectType type;
        std::string authorisation_id; // for a task/document: the Authorisation it belongs to
        bool failed = false; // its own CREATE was refused (Annex D 3018 for anything linking to it)
    };

    // The Authorisation an object's AssociatedObjects resolve to (own id for an Authorisation).
    // nullopt + `why` when a link does not resolve.
    std::optional<std::string>
    resolve_authorisation(const hi1::Object& obj,
                          const std::string& lea,
                          const std::map<std::string, MessageObject>& in_message,
                          ErrorCode& code,
                          std::string& why) {
        if (obj.type() == ObjectType::Authorisation) {
            return obj.identifier();
        }
        std::optional<std::string> found;
        for (const auto& id : obj.associated_objects()) {
            if (const auto it = in_message.find(id); it != in_message.end()) {
                if (it->second.failed) {
                    code = ErrorCode::LinkTargetFailed;
                    why = "AssociatedObject " + id + " was refused in this same request";
                    return std::nullopt;
                }
                if (it->second.type == ObjectType::Authorisation) {
                    found = id;
                } else if (!it->second.authorisation_id.empty()) {
                    found = it->second.authorisation_id;
                }
                continue;
            }
            const auto stored = store.get(id);
            if (!stored || stored->lea != lea) {
                code = ErrorCode::LinkTargetDoesNotExist;
                why = "AssociatedObject " + id + " does not exist";
                return std::nullopt;
            }
            if (stored->status == "Expired") {
                code = ErrorCode::LinkTargetExpired;
                why = "AssociatedObject " + id + " has expired";
                return std::nullopt;
            }
            if (stored->object_type == "Authorisation") {
                found = id;
            } else if (!stored->authorisation_id.empty()) {
                found = stored->authorisation_id;
            }
        }
        if (!found) {
            code = ErrorCode::RequiredElementMissing;
            why = "AssociatedObjects must reference the Authorisation this object belongs to";
        }
        return found;
    }

    struct Planned {
        std::uint64_t action_id = 0;
        StoreOp op;
        hi1::Object object; // what to return to the LEA
        bool is_create = true;
    };

    // Validate and prepare one CREATE. Returns a failure result or fills `planned`.
    std::optional<hi1::ActionResult>
    prepare_create(Workflow wf,
                   std::uint64_t action_id,
                   const hi1::Object& in,
                   const hi1::EndpointId& lea,
                   const std::string& txn,
                   std::map<std::string, MessageObject>& in_message,
                   std::vector<Planned>& planned) {
        const std::string lea_id = lea_key(lea);
        const ObjectType type = in.type();
        if (type == ObjectType::Notification) {
            return failure(action_id,
                           ErrorCode::ImproperValue,
                           "NotificationObjects are created by the Receiver only (7.4.1)");
        }
        if (type == ObjectType::Other) {
            return failure(action_id,
                           ErrorCode::FeatureNotSupported,
                           "object type " + in.type_name() +
                               " is not part of the LI lifecycle workflow profile");
        }
        const auto members = in.members();
        const auto has = [&](const char* m) {
            return std::find(members.begin(), members.end(), m) != members.end();
        };
        if (has("Generation")) {
            return failure(action_id,
                           ErrorCode::ImproperValue,
                           "Generation must not be specified in a CREATE (7.1.3)");
        }
        if (has(status_member(type))) {
            return failure(action_id,
                           ErrorCode::ImproperValue,
                           std::string(status_member(type)) +
                               " is set by the Receiver, not the Sender (7.2.5/8.2.3)");
        }
        if (in_message.count(in.identifier()) != 0 || store.get(in.identifier()).has_value()) {
            return failure(action_id,
                           ErrorCode::ObjectAlreadyExists,
                           "object " + in.identifier() + " already exists");
        }
        if (type == ObjectType::Document) {
            if (const auto ct = in.text_at({"DocumentBody", "ContentType"});
                ct && !acceptable_content_type(*ct)) {
                return failure(action_id,
                               ErrorCode::ImproperValue,
                               "document ContentType " + *ct + " is not accepted (H.5.2.3.4)");
            }
        }
        ErrorCode code = ErrorCode::GeneralBusinessLogicError;
        std::string why;
        std::string authorisation_id;
        if (type == ObjectType::Authorisation) {
            authorisation_id = in.identifier();
        } else {
            const auto resolved = resolve_authorisation(in, lea_id, in_message, code, why);
            if (!resolved) {
                return failure(action_id, code, why);
            }
            authorisation_id = *resolved;
        }
        if (wf == Workflow::TaskAddition) {
            const auto auth = store.get(authorisation_id);
            if (!auth || auth->lea != lea_id) {
                return failure(action_id,
                               ErrorCode::LinkTargetDoesNotExist,
                               "Task Addition needs an existing Authorisation");
            }
            if (terminal(auth->status)) {
                return failure(action_id,
                               ErrorCode::LinkTargetExpired,
                               "Authorisation " + authorisation_id + " is " + auth->status +
                                   " and takes no new tasks");
            }
        }

        hi1::Object obj = in;
        const std::string now = now_timestamp();
        (void)obj.set_text("Generation", "1");
        (void)obj.set_entry(status_member(type), status_entry(type, "AwaitingApproval"));
        (void)obj.set_text("LastChanged", now);
        if (!obj.text("CountryCode")) {
            (void)obj.set_text("CountryCode", lea.country_code);
        }
        if (!obj.text("OwnerIdentifier")) {
            (void)obj.set_text("OwnerIdentifier", lea.unique_identifier);
        }
        if (type == ObjectType::Authorisation) {
            (void)obj.set_text("AuthorisationServedTimestamp", now);
        }
        StoredObject stored;
        project(stored, obj, type);
        stored.authorisation_id = authorisation_id;
        stored.lea = lea_id;
        stored.last_txn = txn;

        in_message[obj.identifier()] = {type, authorisation_id};
        planned.push_back({action_id, {StoreOp::Kind::Insert, stored, 0}, obj, true});
        return std::nullopt;
    }

    std::optional<hi1::ActionResult>
    prepare_update(Workflow wf,
                   std::uint64_t action_id,
                   const hi1::Object& in,
                   const hi1::EndpointId& lea,
                   const std::string& txn,
                   const std::map<std::string, MessageObject>& in_message,
                   std::vector<Planned>& planned,
                   std::string& extension_auth_end,
                   std::string& extension_auth_id) {
        const std::string lea_id = lea_key(lea);
        const auto stored = store.get(in.identifier());
        if (!stored || stored->lea != lea_id) {
            return failure(action_id,
                           ErrorCode::UpdateObjectDoesNotExist,
                           "object " + in.identifier() + " does not exist");
        }
        if (stored->object_type != type_text(in.type())) {
            return failure(action_id,
                           ErrorCode::ImproperValue,
                           "object " + in.identifier() + " is a " + stored->object_type +
                               ", not a " + type_text(in.type()));
        }
        if (stored->status == "Expired" || stored->status == "Cancelled" ||
            stored->status == "Rejected") {
            return failure(action_id,
                           ErrorCode::UpdateObjectExpired,
                           "object " + in.identifier() + " is " + stored->status +
                               " and cannot be updated");
        }
        const ObjectType type = in.type();
        const auto members = in.members();
        const auto has = [&](const char* m) {
            return std::find(members.begin(), members.end(), m) != members.end();
        };
        if (has(status_member(type))) {
            return failure(action_id,
                           ErrorCode::ValueChangeNotAllowed,
                           std::string(status_member(type)) +
                               " is set by the Receiver, not the Sender (7.2.5/8.2.3)");
        }
        if (const auto g = in.text("Generation")) {
            if (std::strtoull(g->c_str(), nullptr, 10) != stored->generation) {
                return failure(action_id,
                               ErrorCode::ImproperValueChange,
                               "Generation " + *g + " does not match the current " +
                                   std::to_string(stored->generation) + " (7.1.3)");
            }
        }

        // The workflow-specific constraints (tables H.1 - H.5).
        const auto desired = in.entry(desired_member(type));
        switch (wf) {
            case Workflow::AuthorisationExtension:
                if (type == ObjectType::Authorisation) {
                    const auto v = hi1::view_authorisation(in);
                    if (!v.end_time || v.start_time) {
                        return failure(action_id,
                                       ErrorCode::RequiredElementMissing,
                                       "AuthorisationTimespan must carry the new EndTime and "
                                       "nothing else (table H.1)");
                    }
                    const auto old_end =
                        hi1::view_authorisation(*hi1::Object::from_xml(stored->xml)).end_time;
                    const auto new_t = parse_time(*v.end_time);
                    const auto old_t = old_end ? parse_time(*old_end) : std::nullopt;
                    if (!new_t || (old_t && *new_t <= *old_t)) {
                        return failure(action_id,
                                       ErrorCode::ImproperValueChange,
                                       "an extension must move the EndTime later");
                    }
                    extension_auth_end = *v.end_time;
                    extension_auth_id = in.identifier();
                } else if (type == ObjectType::LITask) {
                    const auto v = hi1::view_litask(in);
                    if (!v.end_time || v.start_time) {
                        return failure(
                            action_id,
                            ErrorCode::RequiredElementMissing,
                            "Timespan must carry the new EndTime and nothing else (table H.2)");
                    }
                    if (stored->authorisation_id != extension_auth_id &&
                        !extension_auth_id.empty()) {
                        return failure(action_id,
                                       ErrorCode::ImproperValue,
                                       "the task does not belong to the extended Authorisation");
                    }
                    const auto t_end = parse_time(*v.end_time);
                    const auto a_end = parse_time(extension_auth_end);
                    if (!t_end || (a_end && *t_end > *a_end)) {
                        return failure(action_id,
                                       ErrorCode::ImproperValue,
                                       "task EndTime must not be later than the Authorisation's "
                                       "new EndTime (table H.2)");
                    }
                }
                break;
            case Workflow::AuthorisationCancellation:
            case Workflow::TaskCancellation:
                if (!desired || desired->value != "Cancelled") {
                    return failure(action_id,
                                   ErrorCode::RequiredElementMissing,
                                   std::string(desired_member(type)) +
                                       " must be set to \"Cancelled\" (tables H.3 / H.4)");
                }
                break;
            case Workflow::ChangeOfDelivery:
                if (type == ObjectType::LITask && !has("DeliveryDetails")) {
                    return failure(
                        action_id,
                        ErrorCode::RequiredElementMissing,
                        "DeliveryDetails must carry the new delivery details (table H.5)");
                }
                break;
            default:
                break;
        }

        auto merged = hi1::Object::from_xml(stored->xml);
        if (!merged) {
            return failure(action_id,
                           ErrorCode::GeneralBusinessLogicError,
                           "stored object is unreadable: " + merged.error());
        }
        hi1::Object update = in;
        update.remove("Generation");
        if (auto m = merged->merge(update); !m) {
            return failure(action_id, ErrorCode::ImproperValue, m.error());
        }
        (void)merged->set_text("Generation", std::to_string(stored->generation + 1));
        (void)merged->set_text("LastChanged", now_timestamp());
        StoredObject next = *stored;
        project(next, *merged, type);
        next.last_txn = txn;
        (void)in_message;
        planned.push_back(
            {action_id, {StoreOp::Kind::Replace, next, stored->generation}, *merged, false});
        return std::nullopt;
    }

    Outcome handle(Workflow wf,
                   const hi1::EndpointId& lea,
                   const hi1::Header& header,
                   const std::vector<hi1::Action>& actions) {
        Outcome out;
        if (const auto shape = check_shape(wf, actions)) {
            out.rejected = *shape;
            return out;
        }
        if (wf == Workflow::None) {
            for (const auto& a : actions) {
                out.results.push_back(read_action(lea, a));
            }
            return out;
        }

        // Request phase: validate EVERYTHING before writing anything (H.5.2.2.3).
        std::map<std::string, MessageObject> in_message;
        std::vector<Planned> planned;
        std::map<std::uint64_t, hi1::ActionResult> failures;
        std::string ext_end;
        std::string ext_auth;
        // Updates of the Authorisation must be seen before updates of its tasks (extension limits).
        std::vector<const hi1::Action*> ordered;
        for (const auto& a : actions) {
            ordered.push_back(&a);
        }
        std::stable_sort(
            ordered.begin(), ordered.end(), [](const hi1::Action* x, const hi1::Action* y) {
                const auto rank = [](const hi1::Action* a) {
                    if (const auto* u = std::get_if<hi1::UpdateAction>(&a->body)) {
                        return u->object.type() == ObjectType::Authorisation ? 0 : 1;
                    }
                    if (const auto* c = std::get_if<hi1::CreateAction>(&a->body)) {
                        return c->object.type() == ObjectType::Authorisation
                                   ? 0
                                   : (c->object.type() == ObjectType::LITask ? 1 : 2);
                    }
                    return 3;
                };
                return rank(x) < rank(y);
            });
        for (const hi1::Action* a : ordered) {
            std::optional<hi1::ActionResult> bad;
            if (const auto* c = std::get_if<hi1::CreateAction>(&a->body)) {
                bad = prepare_create(
                    wf, a->id, c->object, lea, header.transaction_id, in_message, planned);
                if (bad) {
                    in_message[c->object.identifier()] = {c->object.type(), "", true};
                }
            } else if (const auto* u = std::get_if<hi1::UpdateAction>(&a->body)) {
                bad = prepare_update(wf,
                                     a->id,
                                     u->object,
                                     lea,
                                     header.transaction_id,
                                     in_message,
                                     planned,
                                     ext_end,
                                     ext_auth);
            }
            if (bad) {
                failures.emplace(a->id, std::move(*bad));
            }
        }
        if (!failures.empty()) {
            // Nothing is applied: every action that was itself fine says so, rather than looking
            // accepted.
            for (const auto& a : actions) {
                const auto it = failures.find(a.id);
                out.results.push_back(it != failures.end()
                                          ? it->second
                                          : failure(a.id,
                                                    ErrorCode::GeneralBusinessLogicError,
                                                    "not applied: another action in this request "
                                                    "was refused (H.5.2.2.4)"));
            }
            return out;
        }
        std::vector<StoreOp> ops;
        for (const auto& p : planned) {
            ops.push_back(p.op);
        }
        bool applied = false;
        try {
            applied = store.apply(ops);
        } catch (const std::exception& e) {
            spdlog::error("li-admf: storing an HI1 request failed: {}", e.what());
        }
        if (!applied) {
            out.rejected = top_failure(ErrorCode::TransientTechnicalError,
                                       "the request could not be stored; nothing was changed");
            return out;
        }
        std::map<std::uint64_t, const Planned*> by_action;
        for (const auto& p : planned) {
            by_action[p.action_id] = &p;
        }
        for (const auto& a : actions) {
            const Planned* p = by_action.at(a.id);
            const std::string id = p->object.identifier();
            if (p->is_create) {
                out.results.push_back({a.id, hi1::CreateResult{id, p->object}});
            } else {
                out.results.push_back({a.id, hi1::UpdateResult{id, p->object}});
            }
        }
        wake_worker();
        return out;
    }

    // GET / LIST (and refusal of verbs the base URL does not take).
    hi1::ActionResult read_action(const hi1::EndpointId& lea, const hi1::Action& a) {
        const std::string lea_id = lea_key(lea);
        if (const auto* g = std::get_if<hi1::GetAction>(&a.body)) {
            const auto stored = store.get(g->identifier);
            if (!stored || stored->lea != lea_id) {
                return failure(a.id,
                               ErrorCode::GetObjectNotFound,
                               "object " + g->identifier + " was not found");
            }
            auto obj = hi1::Object::from_xml(stored->xml);
            if (!obj) {
                return failure(
                    a.id, ErrorCode::GeneralBusinessLogicError, "stored object is unreadable");
            }
            return {a.id, hi1::GetResult{*obj}};
        }
        if (const auto* l = std::get_if<hi1::ListAction>(&a.body)) {
            ListFilter f;
            f.lea = lea_id; // an LEA only ever lists its own objects (6.4.8)
            if (l->object_type) {
                f.object_type = l->object_type->value;
            }
            if (l->status) {
                f.status = l->status->value;
            }
            if (l->country_code) {
                f.country_code = *l->country_code;
            }
            if (l->last_changed) {
                f.changed_since = *l->last_changed;
            }
            f.maximum = config.maximum_list_records;
            if (l->maximum_object_count && *l->maximum_object_count < f.maximum) {
                f.maximum = *l->maximum_object_count;
            }
            hi1::ListResult result;
            for (const auto& o : store.list(f)) {
                hi1::ListRecord r;
                r.object_type = {"ETSI", "ObjectType", o.object_type};
                r.identifier = o.object_id;
                if (!o.country_code.empty()) {
                    r.country_code = o.country_code;
                }
                if (!o.owner_identifier.empty()) {
                    r.owner_identifier = o.owner_identifier;
                }
                r.generation = o.generation;
                if (!o.external_id.empty()) {
                    r.external_identifier = o.external_id;
                }
                r.last_changed = o.last_changed;
                result.records.push_back(std::move(r));
            }
            return {a.id, std::move(result)};
        }
        return failure(a.id,
                       ErrorCode::FeatureNotSupported,
                       "this verb is not taken at the API base URL; use a workflow endpoint (H.5)");
    }

    // -------------------------------------------------------------------------------------------
    // review / action phase
    // -------------------------------------------------------------------------------------------

    struct Change {
        std::string object_id;
        ObjectType type;
        std::string status;
        std::string detail;
    };
    using Batch = std::vector<Change>;

    // Set the Receiver-owned status (and an InvalidReason when `reason` is given), bump Generation,
    // and write it back optimistically. nullopt on a conflict (another writer won; retry next
    // pass).
    std::optional<StoredObject> set_status(const StoredObject& s,
                                           const std::string& status,
                                           const std::optional<hi1::Failure>& reason,
                                           Batch& batch) {
        const ObjectType type = s.object_type == "Authorisation" ? ObjectType::Authorisation
                                : s.object_type == "LITask"      ? ObjectType::LITask
                                                                 : ObjectType::Document;
        auto obj = hi1::Object::from_xml(s.xml);
        if (!obj) {
            return std::nullopt;
        }
        (void)obj->set_entry(status_member(type), status_entry(type, status));
        if (reason) {
            (void)obj->set_failure(invalid_member(type), reason->code, reason->description);
        } else {
            obj->remove(invalid_member(type));
        }
        (void)obj->set_text("Generation", std::to_string(s.generation + 1));
        (void)obj->set_text("LastChanged", now_timestamp());
        if (type == ObjectType::Authorisation && terminal(status)) {
            (void)obj->set_text("AuthorisationTerminationTimestamp", now_timestamp());
        }
        StoredObject next = s;
        project(next, *obj, type);
        if (!store.apply({{StoreOp::Kind::Replace, next, s.generation}})) {
            return std::nullopt;
        }
        batch.push_back({s.object_id, type, status, reason ? reason->description : std::string()});
        return next;
    }

    struct Derived {
        std::optional<TaskSpec> spec;
        std::string invalid; // why the task cannot be turned into a provisioning request
    };

    static li_core::x1::TargetIdentifier map_target(const hi1::TargetValue& v) {
        using K = li_core::x1::TargetIdentifierKind;
        const std::string& n = v.format_name;
        if (n == "SUPIIMSI") {
            return {K::SupiImsi, "supiimsi", v.value};
        }
        if (n == "SUPINAI") {
            return {K::SupiNai, "supinai", v.value};
        }
        if (n == "IMSI") {
            return {K::Imsi, "imsi", v.value};
        }
        if (n == "NAI") {
            return {K::Nai, "nai", v.value};
        }
        return {K::Other, n, v.value}; // Lipf::infeasible names it and refuses
    }

    Derived derive(const StoredObject& t) const {
        Derived d;
        auto obj = hi1::Object::from_xml(t.xml);
        if (!obj) {
            d.invalid = "the stored task is unreadable";
            return d;
        }
        const auto v = hi1::view_litask(*obj);
        if (!v.liid || v.liid->empty()) {
            d.invalid = "the task has no Reference (the LIID)";
            return d;
        }
        if (v.targets.empty()) {
            d.invalid = "the task has no TargetIdentifier";
            return d;
        }
        if (!v.delivery_type) {
            d.invalid = "the task has no DeliveryType";
            return d;
        }
        TaskSpec spec;
        spec.xid = t.object_id;
        spec.liid = *v.liid;
        const std::string& dt = v.delivery_type->value;
        if (dt == "IRIOnly") {
            spec.wants_iri = true;
            spec.wants_cc = false;
        } else if (dt == "CCOnly") {
            spec.wants_iri = false;
            spec.wants_cc = true;
        } else if (dt == "IRIandCC") {
            spec.wants_iri = true;
            spec.wants_cc = true;
        } else {
            d.invalid = "DeliveryType " + dt + " is not a TaskDeliveryType value";
            return d;
        }
        for (const auto& tv : v.targets) {
            spec.targets.push_back(map_target(tv));
        }
        for (const auto& dest : v.destinations) {
            if (dest.address_kind != "IPAddressPort") {
                d.invalid = "delivery address kind " + dest.address_kind +
                            " is not supported (IPAddressPort only)";
                return d;
            }
            spec.destinations.push_back({dest.address});
        }
        if (v.start_time) {
            spec.start_time = to_microsecond(*v.start_time);
        }
        if (v.end_time) {
            spec.end_time = to_microsecond(*v.end_time);
        }
        d.spec = std::move(spec);
        return d;
    }

    std::string desired_of(const StoredObject& s, ObjectType type) const {
        auto obj = hi1::Object::from_xml(s.xml);
        if (!obj) {
            return "";
        }
        return obj->entry(desired_member(type)).value_or(hi1::DictionaryEntry{}).value;
    }

    // Take a provisioned task off the network and record `status`. Retried next pass on failure.
    void retire_task(const StoredObject& t,
                     const std::string& status,
                     const std::string& why,
                     Batch& batch) {
        const auto state = store.lipf_state(t.object_id);
        if (state && state->state == "provisioned") {
            TaskSpec spec;
            spec.xid = t.object_id;
            for (const auto& a : state->destinations) {
                spec.destinations.push_back({a});
            }
            const auto r = lipf.deprovision(spec);
            if (!r.ok) {
                spdlog::error("li-admf: deprovisioning task {} failed: {}", t.object_id, r.detail);
                if (t.status != "Error") {
                    (void)set_status(t,
                                     "Error",
                                     hi1::Failure{3003, "could not deprovision: " + r.detail},
                                     batch);
                }
                return;
            }
            store.set_lipf_state(
                {t.object_id, state->provisioned_generation, {}, "deprovisioned", 0});
        }
        if (t.status != status) {
            (void)set_status(t,
                             status,
                             why.empty() ? std::nullopt : std::optional(hi1::Failure{3012, why}),
                             batch);
        }
    }

    // A task of an approved authorisation: make the network match what the LEA asked for.
    void
    action_task(const StoredObject& t, const StoredObject& auth, std::time_t now, Batch& batch) {
        const std::string desired = desired_of(t, ObjectType::LITask);
        if (auth.status == "Cancelled") {
            return retire_task(t, "Cancelled", "", batch);
        }
        if (auth.status == "Rejected") {
            return retire_task(t, "Rejected", "", batch);
        }
        if (auth.status == "Expired") {
            return retire_task(t, "Expired", "", batch);
        }
        if (desired == "Cancelled" || desired == "Rejected" || desired == "Expired") {
            return retire_task(t, desired, "", batch);
        }
        if (auth.status == "Suspended" || desired == "Suspended") {
            return retire_task(t, "Suspended", "", batch);
        }
        if (auth.status != "Approved" && auth.status != "EmergencyApproval") {
            return; // not approved yet: nothing to provision
        }
        if (desired != "" && desired != "Active" && desired != "AwaitingProvisioning") {
            return;
        }

        const auto derived = derive(t);
        auto obj = hi1::Object::from_xml(t.xml);
        const auto view = obj ? hi1::view_litask(*obj) : hi1::LiTaskView{};
        if (view.end_time) {
            if (const auto end = parse_time(*view.end_time); end && *end <= now) {
                return retire_task(t, "Expired", "", batch);
            }
        }
        if (!derived.spec) {
            if (t.status != "Invalid") {
                (void)set_status(t, "Invalid", hi1::Failure{3007, derived.invalid}, batch);
            }
            return;
        }
        if (const auto why = lipf.infeasible(*derived.spec)) {
            const std::string status = t.status == "Active" ? "Error" : "Rejected";
            if (t.status != status) {
                (void)set_status(t, status, hi1::Failure{3001, *why}, batch);
            }
            return;
        }
        if (view.start_time) {
            if (const auto start = parse_time(*view.start_time); start && *start > now) {
                if (t.status != "AwaitingProvisioning") {
                    (void)set_status(t, "AwaitingProvisioning", std::nullopt, batch);
                }
                return;
            }
        }

        const auto state = store.lipf_state(t.object_id);
        const bool in_sync = state && state->state == "provisioned" &&
                             state->provisioned_generation == t.generation && t.status == "Active";
        if (in_sync) {
            return;
        }
        if (t.status == "Error" && state && state->state == "failed" &&
            state->provisioned_generation == t.generation &&
            std::chrono::seconds(state->age_seconds) < config.retry_interval) {
            return; // back off
        }

        const auto r = lipf.provision(*derived.spec);
        if (!r.ok) {
            spdlog::error("li-admf: provisioning task {} failed: {}", t.object_id, r.detail);
            StoredObject current = t;
            if (t.status != "Error") {
                const auto next = set_status(t, "Error", hi1::Failure{3003, r.detail}, batch);
                if (!next) {
                    return;
                }
                current = *next;
            }
            store.set_lipf_state({t.object_id, current.generation, {}, "failed", 0});
            return;
        }
        // Retire destinations a delivery change made obsolete.
        if (state && state->state == "provisioned") {
            std::vector<std::string> gone;
            for (const auto& old : state->destinations) {
                const bool kept =
                    std::any_of(derived.spec->destinations.begin(),
                                derived.spec->destinations.end(),
                                [&](const Destination& d) { return d.address == old; });
                if (!kept) {
                    gone.push_back(old);
                }
            }
            if (!gone.empty()) {
                lipf.retire_destinations(t.object_id, gone);
            }
        }
        StoredObject current = t;
        if (t.status != "Active") {
            const auto next = set_status(t, "Active", std::nullopt, batch);
            if (!next) {
                return; // conflict: provisioning is idempotent, the next pass records it
            }
            current = *next;
        } else {
            batch.push_back({t.object_id, ObjectType::LITask, "Active", "task updated"});
        }
        std::vector<std::string> addresses;
        for (const auto& d : derived.spec->destinations) {
            addresses.push_back(d.address);
        }
        store.set_lipf_state({t.object_id, current.generation, addresses, "provisioned", 0});
    }

    void emit_notification(const StoredObject& auth, const Batch& batch) {
        if (batch.empty()) {
            return;
        }
        hi1::NotificationParams p;
        p.identifier = new_uuid();
        p.country_code = config.self.country_code;
        p.owner_identifier = config.self.unique_identifier;
        p.type = {"ETSI", "NotificationType", "General"};
        p.timestamp = now_timestamp();
        p.associated.push_back(auth.object_id);
        std::string details = "Authorisation " + auth.object_id + ":";
        for (const auto& c : batch) {
            if (c.object_id != auth.object_id) {
                p.associated.push_back(c.object_id);
            }
            hi1::AssociatedStatus s;
            s.object = c.object_id;
            s.status = status_entry(c.type, c.status);
            if (!c.detail.empty()) {
                s.details = c.detail;
            }
            p.statuses.push_back(std::move(s));
            details += " " + std::string(type_text(c.type)) + " " + c.object_id + " -> " +
                       c.status + (c.detail.empty() ? ";" : " (" + c.detail + ");");
        }
        if (!auth.last_txn.empty()) {
            details += " [request transaction " + auth.last_txn + "]";
        }
        p.details = details;
        auto notification = hi1::make_notification(p);
        if (!notification) {
            spdlog::error("li-admf: could not build a NotificationObject: {}",
                          notification.error());
            return;
        }
        (void)notification->set_text("Generation", "1");
        (void)notification->set_text("LastChanged", now_timestamp());
        StoredObject s;
        project(s, *notification, ObjectType::Notification);
        s.authorisation_id = auth.object_id;
        s.lea = auth.lea;
        store.apply({{StoreOp::Kind::Insert, s, 0}});
    }

    // The initial review of a newly served authorisation, atomic with its initial tasks (H.5.2.2.4:
    // if some changes cannot take place, all are rejected).
    void review_new(const StoredObject& auth, const std::string& desired, Batch& batch) {
        ListFilter tf;
        tf.object_type = "LITask";
        tf.authorisation_id = auth.object_id;
        tf.status = "AwaitingApproval";
        const auto tasks = store.list(tf);
        std::string refusal;
        for (const auto& t : tasks) {
            const auto d = derive(t);
            if (!d.spec) {
                refusal = "task " + t.object_id + ": " + d.invalid;
            } else if (const auto why = lipf.infeasible(*d.spec)) {
                refusal = "task " + t.object_id + ": " + *why;
            }
            if (!refusal.empty()) {
                break;
            }
        }
        ListFilter df;
        df.object_type = "Document";
        df.authorisation_id = auth.object_id;
        df.status = "AwaitingApproval";
        const auto docs = store.list(df);
        if (!refusal.empty()) {
            if (const auto rejected =
                    set_status(auth, "Rejected", hi1::Failure{3001, refusal}, batch)) {
                for (const auto& t : tasks) {
                    (void)set_status(t, "Rejected", hi1::Failure{3001, refusal}, batch);
                }
                for (const auto& d : docs) {
                    (void)set_status(d, "Rejected", std::nullopt, batch);
                }
            }
            return;
        }
        if (set_status(auth,
                       desired == "EmergencyApproval" ? "EmergencyApproval" : "Approved",
                       std::nullopt,
                       batch)) {
            for (const auto& d : docs) {
                (void)set_status(d, "Approved", std::nullopt, batch);
            }
        }
    }

    void action_authorisation(const StoredObject& auth, std::time_t now, Batch& batch) {
        auto obj = hi1::Object::from_xml(auth.xml);
        if (!obj) {
            return;
        }
        const auto view = hi1::view_authorisation(*obj);
        const std::string desired = view.desired_status ? view.desired_status->value : "";
        if (view.end_time) {
            if (const auto end = parse_time(*view.end_time); end && *end <= now) {
                (void)set_status(auth, "Expired", std::nullopt, batch);
                return;
            }
        }
        if (desired == "Cancelled" || desired == "Rejected") {
            (void)set_status(auth, desired, std::nullopt, batch);
            return;
        }
        if (desired == "Suspended") {
            if (auth.status != "Suspended") {
                (void)set_status(auth, "Suspended", std::nullopt, batch);
            }
            return;
        }
        if (desired.empty() || desired == "Approved" || desired == "EmergencyApproval" ||
            desired == "SubmittedToCSP") {
            if (auth.status == "AwaitingApproval") {
                review_new(auth, desired, batch);
            } else if (auth.status == "Suspended") {
                (void)set_status(auth,
                                 desired == "EmergencyApproval" ? "EmergencyApproval" : "Approved",
                                 std::nullopt,
                                 batch);
            }
        }
    }

    void reconcile() {
        const std::lock_guard<std::mutex> lock(reconcile_mutex);
        const std::time_t now = std::time(nullptr);
        ListFilter af;
        af.object_type = "Authorisation";
        for (const auto& listed : store.list(af)) {
            if (terminal(listed.status)) {
                // Its tasks may still need retiring (cancel/expire just happened last pass).
            }
            Batch batch;
            StoredObject auth = listed;
            if (!terminal(auth.status)) {
                action_authorisation(auth, now, batch);
                if (const auto fresh = store.get(auth.object_id)) {
                    auth = *fresh;
                }
            }
            ListFilter tf;
            tf.object_type = "LITask";
            tf.authorisation_id = auth.object_id;
            for (const auto& t : store.list(tf)) {
                if (terminal(t.status)) {
                    continue;
                }
                action_task(t, auth, now, batch);
            }
            emit_notification(auth, batch);
        }
    }

    // -------------------------------------------------------------------------------------------
    // worker
    // -------------------------------------------------------------------------------------------

    void wake_worker() {
        {
            const std::lock_guard<std::mutex> lock(wake_mutex);
            woken = true;
        }
        wake_cv.notify_one();
    }

    void keepalive() {
        for (const auto& r : lipf.keepalive_all()) {
            if (r.ok) {
                spdlog::debug("li-admf: X1 keepalive to {} acknowledged", r.ne);
            } else {
                // The NE will raise a fault after its TIME_P2 and, by default, deactivate its
                // tasks.
                spdlog::error("li-admf: X1 keepalive to {} FAILED: {}", r.ne, r.detail);
            }
        }
    }

    void worker_loop() {
        std::unique_lock<std::mutex> lock(wake_mutex);
        auto next_keepalive = std::chrono::steady_clock::now(); // first one immediately: the NEs'
                                                                // P2 clocks started at boot
        while (running.load()) {
            wake_cv.wait_for(
                lock, config.reconcile_interval, [this] { return woken || !running.load(); });
            if (!running.load()) {
                return;
            }
            woken = false;
            lock.unlock();
            try {
                if (std::chrono::steady_clock::now() >= next_keepalive) {
                    keepalive();
                    next_keepalive = std::chrono::steady_clock::now() + config.keepalive_interval;
                }
                reconcile();
            } catch (const std::exception& e) {
                spdlog::error("li-admf: lifecycle reconcile failed: {}", e.what());
            }
            lock.lock();
        }
    }

    // ReportTaskIssue (6.5.2): the NE tells the ADMF something happened to a task it holds.
    std::optional<li_core::x1::ErrorCode> task_issue(const li_core::x1::ReportTaskIssue& report) {
        const auto task = store.get(report.xid);
        if (!task || task->object_type != "LITask") {
            return li_core::x1::ErrorCode::XidDoesNotExist;
        }
        using R = li_core::x1::TaskReportType;
        const std::string detail = report.details.value_or("");
        spdlog::warn("li-admf: NE reported on task {}: type {} code {} {}",
                     report.xid,
                     static_cast<int>(report.report_type),
                     report.error_code.value_or(0),
                     detail);
        std::optional<std::string> new_status;
        std::optional<hi1::Failure> reason;
        switch (report.report_type) {
            case R::TerminatingFault:
            case R::FullyActionedAndUnsuccessful:
                new_status = "Error";
                reason = hi1::Failure{3003,
                                      "the network element reported a fault on this task: " +
                                          (detail.empty() ? std::string("no details") : detail)};
                break;
            case R::ImplicitDeactivation:
                new_status = "Expired"; // the NE stopped intercepting by itself (end time reached)
                break;
            case R::AllClear:
            case R::Warning:
            case R::NonTerminatingFault:
            case R::FullyActionedAndSuccessful:
                break; // recorded in the log; nothing about the task's state changes
        }
        if (!new_status || task->status == *new_status) {
            return std::nullopt;
        }
        const auto auth = store.get(task->authorisation_id);
        Batch batch;
        if (const auto next = set_status(*task, *new_status, reason, batch)) {
            // The network no longer holds it: make the next pass re-provision an Error task, and
            // not retire an Expired one twice.
            store.set_lipf_state({task->object_id,
                                  next->generation,
                                  {},
                                  *new_status == "Error" ? "failed" : "deprovisioned",
                                  0});
            if (auth) {
                emit_notification(*auth, batch);
            }
        }
        return std::nullopt;
    }

    std::optional<li_core::x1::ErrorCode> ne_issue(const std::string& ne,
                                                   const li_core::x1::ReportNEIssue& r) {
        // The worst an NE can say is that it lost its ADMF; that is an operator matter, so log it
        // loudly.
        spdlog::error("li-admf: NE {} reported {} (code {}): {}",
                      ne,
                      static_cast<int>(r.type),
                      r.issue_code.value_or(0),
                      r.description);
        return std::nullopt;
    }
};

Lifecycle::Lifecycle(LifecycleConfig config, Hi1Store& store, Lipf& lipf)
    : impl_(std::make_unique<Impl>(std::move(config), store, lipf)) {}

Lifecycle::~Lifecycle() {
    stop();
}

void Lifecycle::start() {
    if (impl_->running.exchange(true)) {
        return;
    }
    impl_->worker = std::thread([this] { impl_->worker_loop(); });
}

void Lifecycle::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->wake_cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void Lifecycle::wake() {
    impl_->wake_worker();
}

Lifecycle::Outcome Lifecycle::handle(Workflow workflow,
                                     const hi1::EndpointId& lea,
                                     const hi1::Header& header,
                                     const std::vector<hi1::Action>& actions) {
    return impl_->handle(workflow, lea, header, actions);
}

void Lifecycle::keepalive_once() {
    impl_->keepalive();
}

std::optional<li_core::x1::ErrorCode>
Lifecycle::task_issue(const li_core::x1::ReportTaskIssue& report) {
    return impl_->task_issue(report);
}

std::optional<li_core::x1::ErrorCode>
Lifecycle::ne_issue(const std::string& ne, const li_core::x1::ReportNEIssue& report) {
    return impl_->ne_issue(ne, report);
}

void Lifecycle::reconcile_once() {
    impl_->reconcile();
}

} // namespace li_admf
