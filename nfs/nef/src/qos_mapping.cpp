#include "qos_mapping.hpp"

#include <algorithm>

namespace nef {

namespace {

void add_once(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) {
        v.push_back(s);
    }
}

} // namespace

PcfEventPlan pcf_events_for(const sbi_gen::AsSessionWithQoSSubscription& sub) {
    using Af = sbi_gen::UserPlaneEvent;
    using Pcf = sbi_gen::AfEvent_Npcf_PolicyAuthorization;
    PcfEventPlan plan;
    add_once(plan.pcf_events, Pcf::SUCCESSFUL_RESOURCES_ALLOCATION);
    add_once(plan.pcf_events, Pcf::FAILED_RESOURCES_ALLOCATION);
    for (const auto& event : sub.events.value_or(std::vector<Af>{})) {
        const auto& e = event.value;
        if (e == Af::QOS_GUARANTEED || e == Af::QOS_NOT_GUARANTEED ||
            e == Af::QOS_NOT_GUARANTEED_DL || e == Af::QOS_NOT_GUARANTEED_UL) {
            add_once(plan.pcf_events, Pcf::QOS_NOTIF);
        } else if (e == Af::USAGE_REPORT || e == Af::ACCESS_TYPE_CHANGE || e == Af::PLMN_CHG) {
            add_once(plan.pcf_events, e); // the same value in both enums
        } else if (e == Af::SUCCESSFUL_RESOURCES_ALLOCATION ||
                   e == Af::FAILED_RESOURCES_ALLOCATION || e == Af::SESSION_TERMINATION) {
            // Already subscribed, or reported without a subscription.
        } else {
            add_once(plan.not_subscribed, e);
        }
    }
    if (sub.usageThreshold.has_value()) {
        add_once(plan.pcf_events, Pcf::USAGE_REPORT);
    }
    return plan;
}

nlohmann::json pcf_session_state(const sbi_gen::AsSessionWithQoSSubscription& sub) {
    nlohmann::json state = nlohmann::json::object();

    if (sub.flowInfo.has_value() || sub.qosReference.has_value()) {
        sbi_gen::MediaComponent component{};
        component.medCompN = 1;
        component.qosReference = sub.qosReference;
        if (sub.flowInfo.has_value()) {
            nlohmann::json subcomponents = nlohmann::json::object();
            for (const auto& flow : *sub.flowInfo) {
                sbi_gen::MediaSubComponent sc{};
                sc.fNum = flow.flowId;
                sc.fDescs = flow.flowDescriptions;
                subcomponents[std::to_string(flow.flowId)] = sc;
            }
            component.medSubComps = subcomponents;
        }
        state["medComponents"] = nlohmann::json{{"1", component}};
    }
    if (sub.exterAppId.has_value()) {
        state["afAppId"] = *sub.exterAppId;
    }

    sbi_gen::EventsSubscReqData_Npcf_PolicyAuthorization ev{};
    for (const auto& name : pcf_events_for(sub).pcf_events) {
        sbi_gen::AfEventSubscription s{};
        s.event.value = name;
        ev.events.push_back(s);
    }
    ev.usgThres = sub.usageThreshold;
    state["evSubsc"] = ev;
    return state;
}

sbi_gen::AppSessionContextReqData
app_session_request(const sbi_gen::AsSessionWithQoSSubscription& sub,
                    const std::string& nef_notif_uri) {
    nlohmann::json req = pcf_session_state(sub);
    req["notifUri"] = nef_notif_uri;
    req["evSubsc"]["notifUri"] = nef_notif_uri;
    req["suppFeat"] = "0";
    if (sub.ueIpv4Addr.has_value()) {
        req["ueIpv4"] = *sub.ueIpv4Addr;
    }
    if (sub.ueIpv6Addr.has_value()) {
        req["ueIpv6"] = *sub.ueIpv6Addr;
    }
    if (sub.macAddr.has_value()) {
        req["ueMac"] = *sub.macAddr;
    }
    if (sub.dnn.has_value()) {
        req["dnn"] = *sub.dnn;
    }
    if (sub.snssai.has_value()) {
        req["sliceInfo"] = *sub.snssai;
    }
    return req.get<sbi_gen::AppSessionContextReqData>();
}

nlohmann::json merge_patch_between(const nlohmann::json& from, const nlohmann::json& to) {
    if (!from.is_object() || !to.is_object()) {
        return to;
    }
    nlohmann::json patch = nlohmann::json::object();
    for (const auto& [key, value] : from.items()) {
        if (!to.contains(key)) {
            patch[key] = nullptr;
        }
    }
    for (const auto& [key, value] : to.items()) {
        if (!from.contains(key)) {
            patch[key] = value;
        } else if (from.at(key) != value) {
            patch[key] = from.at(key).is_object() && value.is_object()
                             ? merge_patch_between(from.at(key), value)
                             : value;
        }
    }
    return patch;
}

nlohmann::json pcf_update_patch(const sbi_gen::AsSessionWithQoSSubscription& before,
                                const sbi_gen::AsSessionWithQoSSubscription& after) {
    auto patch = merge_patch_between(pcf_session_state(before), pcf_session_state(after));
    if (!patch.contains("medComponents") || !patch.at("medComponents").is_object()) {
        return patch;
    }
    for (auto& [n, component] : patch.at("medComponents").items()) {
        if (!component.is_object()) {
            continue; // null: the component is removed
        }
        component["medCompN"] = std::stoll(n);
        if (component.contains("medSubComps") && component.at("medSubComps").is_object()) {
            for (auto& [f, subcomponent] : component.at("medSubComps").items()) {
                if (subcomponent.is_object()) {
                    subcomponent["fNum"] = std::stoll(f);
                }
            }
        }
    }
    return patch;
}

namespace {

// fNums of media component 1 across `flows`; nullopt when none names a flow number.
void add_flow_ids(std::optional<std::vector<std::int64_t>>& ids,
                  const std::optional<std::vector<sbi_gen::Flows>>& flows) {
    for (const auto& f : flows.value_or(std::vector<sbi_gen::Flows>{})) {
        if (f.medCompN != 1 || !f.fNums.has_value()) {
            continue;
        }
        for (const auto n : *f.fNums) {
            if (!ids.has_value()) {
                ids.emplace();
            }
            if (std::find(ids->begin(), ids->end(), n) == ids->end()) {
                ids->push_back(n);
            }
        }
    }
}

sbi_gen::UserPlaneEventReport report_of(const std::string& event) {
    sbi_gen::UserPlaneEventReport r{};
    r.event.value = event;
    return r;
}

} // namespace

AfReportPlan
af_reports_for(const sbi_gen::EventsNotification_Npcf_PolicyAuthorization& notification) {
    using Af = sbi_gen::UserPlaneEvent;
    using Pcf = sbi_gen::AfEvent_Npcf_PolicyAuthorization;
    AfReportPlan plan;
    for (const auto& ev : notification.evNotifs) {
        const auto& e = ev.event.value;
        if (e == Pcf::SUCCESSFUL_RESOURCES_ALLOCATION || e == Pcf::FAILED_RESOURCES_ALLOCATION) {
            auto r = report_of(e); // the same value in both enums
            const auto& allocs = e == Pcf::SUCCESSFUL_RESOURCES_ALLOCATION
                                     ? notification.succResourcAllocReports
                                     : notification.failedResourcAllocReports;
            for (const auto& a : allocs.value_or(std::vector<sbi_gen::ResourcesAllocationInfo>{})) {
                add_flow_ids(r.flowIds, a.flows);
            }
            add_flow_ids(r.flowIds, ev.flows);
            plan.reports.push_back(r);
        } else if (e == Pcf::QOS_NOTIF) {
            if (!notification.qncReports.has_value()) {
                add_once(plan.not_mapped, e + " without qncReports");
            }
            for (const auto& q : notification.qncReports.value_or(
                     std::vector<sbi_gen::QosNotificationControlInfo_Npcf_PolicyAuthorization>{})) {
                const auto& t = q.notifType.value;
                const std::string af_event =
                    t == "GUARANTEED"          ? std::string(Af::QOS_GUARANTEED)
                    : t == "NOT_GUARANTEED"    ? std::string(Af::QOS_NOT_GUARANTEED)
                    : t == "NOT_GUARANTEED_DL" ? std::string(Af::QOS_NOT_GUARANTEED_DL)
                    : t == "NOT_GUARANTEED_UL" ? std::string(Af::QOS_NOT_GUARANTEED_UL)
                                               : std::string{};
                if (af_event.empty()) {
                    add_once(plan.not_mapped, e + " notifType " + t);
                    continue;
                }
                auto r = report_of(af_event);
                add_flow_ids(r.flowIds, q.flows);
                r.altQosNotSuppInd = q.altSerReqNotSuppInd;
                plan.reports.push_back(r);
            }
        } else if (e == Pcf::USAGE_REPORT) {
            auto r = report_of(e);
            r.accumulatedUsage = notification.usgRep;
            plan.reports.push_back(r);
        } else if (e == Pcf::ACCESS_TYPE_CHANGE) {
            auto r = report_of(e);
            r.ratType = notification.ratType;
            plan.reports.push_back(r);
        } else if (e == Pcf::PLMN_CHG) {
            auto r = report_of(e);
            r.plmnId = notification.plmnId;
            plan.reports.push_back(r);
        } else {
            add_once(plan.not_mapped, e);
        }
    }
    return plan;
}

sbi_gen::UserPlaneEventReport session_termination_report() {
    return report_of(sbi_gen::UserPlaneEvent::SESSION_TERMINATION);
}

} // namespace nef
