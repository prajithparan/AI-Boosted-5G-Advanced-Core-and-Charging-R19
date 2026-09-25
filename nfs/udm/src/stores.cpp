#include "stores.hpp"

namespace udm {

void AmfRegistrationStore::put(const std::string& ue_id, nlohmann::json registration) {
    std::lock_guard<std::mutex> lock(mutex_);
    registrations_[ue_id] = std::move(registration);
}

std::optional<nlohmann::json> AmfRegistrationStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(ue_id);
    if (it == registrations_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

std::optional<nlohmann::json> AmfRegistrationStore::merge_patch(const std::string& ue_id,
                                                                const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(ue_id);
    if (it == registrations_.end()) {
        return std::nullopt;
    }
    it->second.merge_patch(patch);
    return std::make_optional(it->second);
}

bool AmfRegistrationStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return registrations_.erase(ue_id) > 0;
}

void AmfNon3GppRegistrationStore::put(const std::string& ue_id, nlohmann::json registration) {
    std::lock_guard<std::mutex> lock(mutex_);
    registrations_[ue_id] = std::move(registration);
}

std::optional<nlohmann::json> AmfNon3GppRegistrationStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(ue_id);
    if (it == registrations_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

std::optional<nlohmann::json>
AmfNon3GppRegistrationStore::merge_patch(const std::string& ue_id, const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(ue_id);
    if (it == registrations_.end()) {
        return std::nullopt;
    }
    it->second.merge_patch(patch);
    return std::make_optional(it->second);
}

void SmsfRegistrationStore::put(const std::string& ue_id, nlohmann::json registration) {
    std::lock_guard<std::mutex> lock(mutex_);
    registrations_[ue_id] = std::move(registration);
}

std::optional<nlohmann::json> SmsfRegistrationStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(ue_id);
    if (it == registrations_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

std::optional<nlohmann::json> SmsfRegistrationStore::merge_patch(const std::string& ue_id,
                                                                 const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(ue_id);
    if (it == registrations_.end()) {
        return std::nullopt;
    }
    it->second.merge_patch(patch);
    return std::make_optional(it->second);
}

bool SmsfRegistrationStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return registrations_.erase(ue_id) > 0;
}

void IpSmGwRegistrationStore::put(const std::string& ue_id, nlohmann::json registration) {
    std::lock_guard<std::mutex> lock(mutex_);
    registrations_[ue_id] = std::move(registration);
}

std::optional<nlohmann::json> IpSmGwRegistrationStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(ue_id);
    if (it == registrations_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool IpSmGwRegistrationStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return registrations_.erase(ue_id) > 0;
}

void RoamingInfoUpdateStore::put(const std::string& ue_id, nlohmann::json info) {
    std::lock_guard<std::mutex> lock(mutex_);
    info_[ue_id] = std::move(info);
}

std::optional<nlohmann::json> RoamingInfoUpdateStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = info_.find(ue_id);
    if (it == info_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

void SmfRegistrationStore::put(const std::string& ue_id,
                               const std::string& pdu_session_id,
                               nlohmann::json registration) {
    std::lock_guard<std::mutex> lock(mutex_);
    registrations_[ue_id][pdu_session_id] = std::move(registration);
}

std::optional<nlohmann::json> SmfRegistrationStore::get(const std::string& ue_id,
                                                        const std::string& pdu_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return std::nullopt;
    }
    auto session_it = ue_it->second.find(pdu_session_id);
    if (session_it == ue_it->second.end()) {
        return std::nullopt;
    }
    return std::make_optional(session_it->second);
}

std::optional<nlohmann::json> SmfRegistrationStore::merge_patch(const std::string& ue_id,
                                                                const std::string& pdu_session_id,
                                                                const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return std::nullopt;
    }
    auto session_it = ue_it->second.find(pdu_session_id);
    if (session_it == ue_it->second.end()) {
        return std::nullopt;
    }
    session_it->second.merge_patch(patch);
    return std::make_optional(session_it->second);
}

bool SmfRegistrationStore::remove(const std::string& ue_id, const std::string& pdu_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return false;
    }
    return ue_it->second.erase(pdu_session_id) > 0;
}

std::vector<nlohmann::json> SmfRegistrationStore::list_for_ue(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> result;
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return result;
    }
    result.reserve(ue_it->second.size());
    for (const auto& [pdu_session_id, registration] : ue_it->second) {
        result.push_back(registration);
    }
    return result;
}

void NwdafRegistrationStore::put(const std::string& ue_id,
                                 const std::string& nwdaf_registration_id,
                                 nlohmann::json registration) {
    std::lock_guard<std::mutex> lock(mutex_);
    registrations_[ue_id][nwdaf_registration_id] = std::move(registration);
}

std::optional<nlohmann::json>
NwdafRegistrationStore::get(const std::string& ue_id, const std::string& nwdaf_registration_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return std::nullopt;
    }
    auto reg_it = ue_it->second.find(nwdaf_registration_id);
    if (reg_it == ue_it->second.end()) {
        return std::nullopt;
    }
    return std::make_optional(reg_it->second);
}

std::optional<nlohmann::json>
NwdafRegistrationStore::merge_patch(const std::string& ue_id,
                                    const std::string& nwdaf_registration_id,
                                    const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return std::nullopt;
    }
    auto reg_it = ue_it->second.find(nwdaf_registration_id);
    if (reg_it == ue_it->second.end()) {
        return std::nullopt;
    }
    reg_it->second.merge_patch(patch);
    return std::make_optional(reg_it->second);
}

bool NwdafRegistrationStore::remove(const std::string& ue_id,
                                    const std::string& nwdaf_registration_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return false;
    }
    return ue_it->second.erase(nwdaf_registration_id) > 0;
}

std::vector<nlohmann::json> NwdafRegistrationStore::list_for_ue(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> result;
    auto ue_it = registrations_.find(ue_id);
    if (ue_it == registrations_.end()) {
        return result;
    }
    result.reserve(ue_it->second.size());
    for (const auto& [nwdaf_registration_id, registration] : ue_it->second) {
        result.push_back(registration);
    }
    return result;
}

std::string SdmSubscriptionStore::create(SdmSubscriptionEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "sdmsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(entry));
    return id;
}

std::optional<SdmSubscriptionEntry> SdmSubscriptionStore::get(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(subscription_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool SdmSubscriptionStore::remove(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(subscription_id) > 0;
}

std::optional<SdmSubscriptionEntry> SdmSubscriptionStore::merge_patch(
    const std::string& subscription_id, const std::string& ue_id, const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(subscription_id);
    if (it == subscriptions_.end() || it->second.ue_id != ue_id) {
        return std::nullopt;
    }
    it->second.data.merge_patch(patch);
    return std::make_optional(it->second);
}

std::string SharedDataSubscriptionStore::create(nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "shdsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(data));
    return id;
}

std::optional<nlohmann::json> SharedDataSubscriptionStore::get(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(subscription_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool SharedDataSubscriptionStore::remove(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(subscription_id) > 0;
}

std::optional<nlohmann::json>
SharedDataSubscriptionStore::merge_patch(const std::string& subscription_id,
                                         const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(subscription_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    it->second.merge_patch(patch);
    return std::make_optional(it->second);
}

std::string AuthEventStore::create(const std::string& supi, nlohmann::json event) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "authevent-" + std::to_string(next_id_++);
    events_.emplace(id, Entry{.supi = supi, .event = std::move(event)});
    return id;
}

bool AuthEventStore::remove(const std::string& supi, const std::string& auth_event_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = events_.find(auth_event_id);
    if (it == events_.end() || it->second.supi != supi) {
        return false;
    }
    events_.erase(it);
    return true;
}

bool AuthEventStore::has_successful_event(const std::string& supi) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, entry] : events_) {
        if (entry.supi == supi && entry.event.value("success", false)) {
            return true;
        }
    }
    return false;
}

std::string EeSubscriptionStore::create(EeSubscriptionEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "eesub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(entry));
    return id;
}

std::optional<EeSubscriptionEntry> EeSubscriptionStore::get(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(subscription_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

std::optional<nlohmann::json> EeSubscriptionStore::apply_patch(const std::string& subscription_id,
                                                               const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(subscription_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    it->second.data = it->second.data.patch(patch_ops);
    return std::make_optional(it->second.data);
}

bool EeSubscriptionStore::remove(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(subscription_id) > 0;
}

void PpDataStore::put(const std::string& ue_id, nlohmann::json pp_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pp_data_[ue_id] = std::move(pp_data);
}

std::optional<nlohmann::json> PpDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pp_data_.find(ue_id);
    if (it == pp_data_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

std::optional<nlohmann::json> PpDataStore::merge_patch(const std::string& ue_id,
                                                       const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pp_data_.find(ue_id);
    if (it == pp_data_.end()) {
        // Real spec behavior: PpData is nullable and the resource is a singular per-UE document
        // that may not exist yet -- a merge-patch onto "no document" starts from an empty object,
        // same real RFC 7396 semantics as PATCHing a not-yet-existing resource elsewhere in this
        // project (e.g. AMF's own N1N2 subscription creation-on-demand shape).
        nlohmann::json doc = nlohmann::json::object();
        doc.merge_patch(patch);
        pp_data_[ue_id] = doc;
        return std::make_optional(doc);
    }
    it->second.merge_patch(patch);
    return std::make_optional(it->second);
}

} // namespace udm
