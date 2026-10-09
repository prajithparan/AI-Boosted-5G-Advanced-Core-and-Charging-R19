#include "fl_server.hpp"

#include <algorithm>

#include "fl_client.hpp"

namespace nwdaf {

using json = nlohmann::json;

namespace {

std::string training_base(const json& profile) {
    for (const auto& svc : profile.value("nfServices", json::array())) {
        if (svc.value("serviceName", "") != "nnwdaf-mlmodeltraining") {
            continue;
        }
        for (const auto& ep : svc.value("ipEndPoints", json::array())) {
            if (ep.contains("ipv4Address") && ep.contains("port")) {
                return svc.value("scheme", "https") + "://" +
                       ep.at("ipv4Address").get<std::string>() + ":" +
                       std::to_string(ep.at("port").get<int>());
            }
        }
    }
    return {};
}

bool can_be_client_for(const json& profile, const std::string& event) {
    for (const auto& info :
         profile.value("nwdafInfo", json::object()).value("mlAnalyticsList", json::array())) {
        const auto cap = info.value("flCapabilityType", "");
        if (cap != "FL_CLIENT" && cap != "FL_SERVER_AND_CLIENT") {
            continue;
        }
        const auto ids = info.value("mlAnalyticsIds", json::array());
        if (std::find(ids.begin(), ids.end(), json(event)) != ids.end()) {
            return true;
        }
    }
    return false;
}

json round_members(const FlFederation& fed, int round, const json& global_model) {
    json m{{"roundInd", round},
           {"skipFlInd", false},
           {"mLAccChkFlg", true},
           {"mLTrainRepInfo", json{{"maxResTime", fed.max_res_time_s}}}};
    m["mLModelInfos"] =
        global_model.is_null()
            ? json::array()
            : json::array(
                  {json{{"event", fed.event}, {"mlFile", fl_base64_encode(global_model.dump())}}});
    return m;
}

} // namespace

std::vector<FlClientRef> select_fl_clients(const json& nf_instances,
                                           const std::string& event,
                                           const std::string& self_instance_id) {
    std::vector<FlClientRef> out;
    for (const auto& p : nf_instances) {
        const auto id = p.value("nfInstanceId", "");
        if (id.empty() || id == self_instance_id || !can_be_client_for(p, event)) {
            continue;
        }
        const auto base = training_base(p);
        if (!base.empty()) {
            out.push_back(FlClientRef{id, base});
        }
    }
    return out;
}

json build_round_subscription(const FlFederation& fed,
                              int round,
                              const json& global_model,
                              const std::string& notif_uri,
                              const std::string& notif_corre_id) {
    json sub = round_members(fed, round, global_model);
    sub["mLEventSubscs"] = json::array({json{{"mLEvent", fed.event}}});
    sub["notifUri"] = notif_uri;
    sub["notifCorreId"] = notif_corre_id;
    sub["mlCorreId"] = fed.ml_corre_id;
    if (sub.at("mLModelInfos").empty()) {
        sub.erase("mLModelInfos"); // minItems 1 in the YAML: omit rather than send []
    }
    return sub;
}

json build_round_patch(const FlFederation& fed, int round, const json& global_model) {
    return round_members(fed, round, global_model);
}

tl::expected<FlUpdate, std::string> parse_fl_notification(const json& body,
                                                          const FlFederation& fed,
                                                          int round,
                                                          const std::string& notif_corre_id) {
    if (body.value("notifCorreId", "") != notif_corre_id ||
        body.value("mlCorreId", "") != fed.ml_corre_id) {
        return tl::make_unexpected("notification belongs to another federation");
    }
    if (body.value("roundInd", -1) != round) {
        return tl::make_unexpected("notification is for round " +
                                   std::to_string(body.value("roundInd", -1)) + ", not " +
                                   std::to_string(round));
    }
    if (body.contains("delayEventNotif")) {
        return tl::make_unexpected("client reported " +
                                   body.at("delayEventNotif").value("delayCause", "a delay"));
    }
    if (!body.contains("mLModelInfos") || body.at("mLModelInfos").empty() ||
        !body.at("mLModelInfos").front().contains("mlFile")) {
        return tl::make_unexpected("notification carries no inline interim model");
    }
    std::string raw;
    if (!fl_base64_decode(body.at("mLModelInfos").front().at("mlFile").get<std::string>(), raw)) {
        return tl::make_unexpected("interim model mlFile is not base64");
    }
    FlUpdate u;
    u.model = json::parse(raw, nullptr, false);
    if (u.model.is_discarded() || !u.model.is_object() || !u.model.contains("n_samples") ||
        !u.model.at("n_samples").is_number_integer() || u.model.at("n_samples").get<long>() <= 0) {
        return tl::make_unexpected("interim model is not JSON with a positive integer n_samples");
    }
    u.n_samples = u.model.at("n_samples").get<long>();
    if (body.contains("statusReport") && body.at("statusReport").contains("mlModelAcc")) {
        u.accuracy_of_global = body.at("statusReport").at("mlModelAcc").get<long>();
    }
    return u;
}

} // namespace nwdaf
