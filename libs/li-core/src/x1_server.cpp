#include "li_core/x1_server.hpp"

#include <variant>

namespace li_core::x1 {

namespace {

const char* code_text(ErrorCode c) {
    switch (c) {
        case ErrorCode::UnsupportedRequest:
            return "Unsupported request";
        case ErrorCode::UnexpectedAdmfIdentifier:
            return "Unexpected ADMF Identifier";
        case ErrorCode::UnexpectedNeIdentifier:
            return "Unexpected NE Identifier";
        case ErrorCode::KeepaliveNotSupported:
            return "Keepalive not supported";
        case ErrorCode::XidAlreadyExists:
            return "XID already exists on NE";
        case ErrorCode::XidDoesNotExist:
            return "XID does not exist on NE";
        case ErrorCode::DidAlreadyExists:
            return "DID already exists on the NE";
        case ErrorCode::DidDoesNotExist:
            return "DID does not exist on the NE";
        case ErrorCode::UnsupportedTargetIdentifier:
            return "Unsupported TargetIdentifier type";
        case ErrorCode::DeactivateAllTasksNotEnabled:
            return "DeactivateAllTasks not enabled";
        default:
            return "Request could not be actioned";
    }
}

// The response's envelope: echo the request's, but stamp our NE identifier when we have one and
// the request's was empty.
MessageHeader response_header(const MessageHeader& req, const std::string& ne_id) {
    MessageHeader h = req;
    if (!ne_id.empty()) {
        h.ne_identifier = ne_id;
    }
    return h;
}

} // namespace

std::string handle_request(const std::string& xml, const TaskStoreCallbacks& cb) {
    auto parsed = parse_request(xml);
    if (!parsed) {
        // TopLevelError (6.1): only the envelope, no per-message body.
        MessageHeader h = parsed.error().header.value_or(MessageHeader{});
        if (h.ne_identifier.empty()) {
            h.ne_identifier = cb.ne_identifier;
        }
        return serialise_top_level_error(h);
    }

    std::vector<ResponseItem> items;
    items.reserve(parsed->requests.size());
    for (const auto& req : parsed->requests) {
        const MessageHeader h = response_header(req.header, cb.ne_identifier);
        const auto reject = [&](ErrorCode c, const char* desc) {
            items.push_back(ErrorResponse{h, req.type, c, desc});
        };
        const auto accept = [&](bool completed = true) {
            items.push_back(OkResponse{h, req.type, completed});
        };

        // Identity check first (8.x): a request whose identifiers do not match the peer is
        // rejected before any store action.
        if (cb.check_identity) {
            if (auto err = cb.check_identity(req.header)) {
                reject(*err, code_text(*err));
                continue;
            }
        }

        switch (req.type) {
            case MessageType::ActivateTask: {
                const auto& t = std::get<ActivateTask>(req.body).task;
                auto err = cb.activate_task
                               ? cb.activate_task(t)
                               : std::optional<ErrorCode>(ErrorCode::ActivateTaskFailure);
                err ? reject(*err, code_text(*err)) : accept();
                break;
            }
            case MessageType::ModifyTask: {
                const auto& t = std::get<ModifyTask>(req.body).task;
                auto err = cb.modify_task ? cb.modify_task(t)
                                          : std::optional<ErrorCode>(ErrorCode::ModifyTaskFailure);
                err ? reject(*err, code_text(*err)) : accept();
                break;
            }
            case MessageType::DeactivateTask: {
                const auto& xid = std::get<DeactivateTask>(req.body).xid;
                auto err = cb.deactivate_task
                               ? cb.deactivate_task(xid)
                               : std::optional<ErrorCode>(ErrorCode::DeactivateTaskFailure);
                err ? reject(*err, code_text(*err)) : accept();
                break;
            }
            case MessageType::DeactivateAllTasks: {
                auto err = cb.deactivate_all_tasks
                               ? cb.deactivate_all_tasks()
                               : std::optional<ErrorCode>(ErrorCode::DeactivateAllTasksNotEnabled);
                err ? reject(*err, code_text(*err)) : accept();
                break;
            }
            case MessageType::CreateDestination: {
                const auto& d = std::get<CreateDestination>(req.body).destination;
                auto err = cb.create_destination
                               ? cb.create_destination(d)
                               : std::optional<ErrorCode>(ErrorCode::CreateDestinationFailure);
                err ? reject(*err, code_text(*err)) : accept();
                break;
            }
            case MessageType::RemoveDestination: {
                const auto& did = std::get<RemoveDestination>(req.body).did;
                auto err = cb.remove_destination
                               ? cb.remove_destination(did)
                               : std::optional<ErrorCode>(ErrorCode::RemoveDestinationFailure);
                err ? reject(*err, code_text(*err)) : accept();
                break;
            }
            case MessageType::RemoveAllDestinations: {
                auto err = cb.remove_all_destinations ? cb.remove_all_destinations()
                                                      : std::optional<ErrorCode>(std::nullopt);
                err ? reject(*err, code_text(*err)) : accept();
                break;
            }
            case MessageType::Ping:
                accept();
                break;
            case MessageType::Keepalive:
                if (cb.keepalive_supported) {
                    accept();
                } else {
                    reject(ErrorCode::KeepaliveNotSupported,
                           code_text(ErrorCode::KeepaliveNotSupported));
                }
                break;
            case MessageType::GetTaskDetails:
            case MessageType::Unsupported:
            default:
                reject(ErrorCode::UnsupportedRequest, code_text(ErrorCode::UnsupportedRequest));
                break;
        }
    }

    auto out = serialise_response(items);
    if (!out) {
        // Building a schema-valid response failed -- a codec bug, not an X1 error. Fall back to a
        // top-level error so the ADMF still gets a well-formed reply.
        MessageHeader h;
        h.ne_identifier = cb.ne_identifier;
        if (!parsed->requests.empty()) {
            h = response_header(parsed->requests.front().header, cb.ne_identifier);
        }
        return serialise_top_level_error(h);
    }
    return *out;
}

// ---- keepalive state machine (6.6.2) ----

KeepaliveMonitor::Action
KeepaliveMonitor::on_x1_request(std::chrono::steady_clock::time_point now) {
    last_x1_ = now;
    seen_first_ = true;
    if (state_ != State::Normal) {
        // Any X1 request clears the fault and returns to normal (6.6.2).
        state_ = State::Normal;
        return Action::SendFaultCleared;
    }
    return Action::None;
}

KeepaliveMonitor::Action
KeepaliveMonitor::on_fault_report_ack(std::chrono::steady_clock::time_point now) {
    if (state_ != State::FaultRaised) {
        return Action::None;
    }
    if (cfg_.allow_deactivate_all) {
        state_ = State::AwaitingDeactivation;
        ack_at_ = now;
        return Action::None;
    }
    // Not allowed to deactivate: reset P2 and carry on.
    state_ = State::Normal;
    last_x1_ = now;
    return Action::None;
}

KeepaliveMonitor::Action KeepaliveMonitor::tick(std::chrono::steady_clock::time_point now) {
    if (!seen_first_) {
        last_x1_ = now; // P2 starts once the interface is in use
        seen_first_ = true;
        return Action::None;
    }
    switch (state_) {
        case State::Normal:
            if (now - last_x1_ >= cfg_.time_p2) {
                state_ = State::FaultRaised;
                fault_at_ = now;
                return Action::SendFaultReport; // code 9050, and start P1
            }
            return Action::None;
        case State::FaultRaised:
            if (now - fault_at_ >= cfg_.time_p1) {
                // P1 expired with no ADMF ack.
                if (cfg_.allow_deactivate_all) {
                    state_ = State::Normal;
                    last_x1_ = now;
                    return Action::DeactivateAllTasks; // + Alert code 10000
                }
                state_ = State::Normal;
                last_x1_ = now;
            }
            return Action::None;
        case State::AwaitingDeactivation:
            if (now - ack_at_ >= cfg_.time_p3) {
                state_ = State::Normal;
                last_x1_ = now;
                return Action::DeactivateAllTasks;
            }
            return Action::None;
    }
    return Action::None;
}

} // namespace li_core::x1
