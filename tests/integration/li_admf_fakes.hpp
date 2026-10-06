#pragma once

// In-memory network elements for the ADMF tests: li_core's REAL X1 server codec (handle_request)
// over a recording task store, so every request the LIPF builds is parsed and schema-validated
// exactly as a real NE would, and every answer is a real X1 response.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "li_core/x1_server.hpp"
#include "lipf.hpp"

namespace li_admf_test {

namespace x1 = li_core::x1;
using li_admf::NetworkElement;

// One in-memory NE: a real X1 server over a recording store, with switchable failure.
struct FakeNe {
    std::map<std::string, x1::TaskDetails> tasks;
    std::map<std::string, x1::DestinationDetails> destinations;
    std::vector<std::string> log; // "ActivateTask xid" ... in arrival order
    std::optional<x1::ErrorCode> fail_activate;

    x1::TaskStoreCallbacks callbacks(const std::string& ne_id) {
        x1::TaskStoreCallbacks cb;
        cb.ne_identifier = ne_id;
        cb.activate_task = [this](const x1::TaskDetails& t) -> std::optional<x1::ErrorCode> {
            log.push_back("ActivateTask " + t.xid);
            if (fail_activate) {
                return fail_activate;
            }
            if (tasks.count(t.xid) != 0) {
                return x1::ErrorCode::XidAlreadyExists;
            }
            tasks[t.xid] = t;
            return std::nullopt;
        };
        cb.modify_task = [this](const x1::TaskDetails& t) -> std::optional<x1::ErrorCode> {
            log.push_back("ModifyTask " + t.xid);
            tasks[t.xid] = t;
            return std::nullopt;
        };
        cb.deactivate_task = [this](const std::string& xid) -> std::optional<x1::ErrorCode> {
            log.push_back("DeactivateTask " + xid);
            if (tasks.erase(xid) == 0) {
                return x1::ErrorCode::XidDoesNotExist;
            }
            return std::nullopt;
        };
        cb.create_destination = [this](const x1::DestinationDetails& d) -> std::optional<x1::ErrorCode> {
            log.push_back("CreateDestination " + d.did);
            if (destinations.count(d.did) != 0) {
                return x1::ErrorCode::DidAlreadyExists;
            }
            destinations[d.did] = d;
            return std::nullopt;
        };
        cb.remove_destination = [this](const std::string& did) -> std::optional<x1::ErrorCode> {
            log.push_back("RemoveDestination " + did);
            return destinations.erase(did) == 0 ? std::optional(x1::ErrorCode::DidDoesNotExist) : std::nullopt;
        };
        return cb;
    }
};

class FakeTransport : public li_admf::X1Transport {
public:
    FakeNe amf;
    FakeNe mdf;
    tl::expected<std::string, std::string> post(const NetworkElement& ne, const std::string& body) override {
        FakeNe& target = ne.role == "mdf2" ? mdf : amf;
        return x1::handle_request(body, target.callbacks(ne.ne_identifier));
    }
};

} // namespace li_admf_test
