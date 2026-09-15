#include "classify.hpp"

#include "TS26510_CommonData_grp.hpp"

namespace mfaf {

namespace {

// The API name is everything before the second underscore-separated component's end:
// "Namf_EventExposure_Notify" -> "Namf_EventExposure".
std::string api_name(std::string_view header) {
    const auto semi = header.find(';'); // "; apiversion=2" is permitted after the type
    std::string_view type = header.substr(0, semi);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    const auto first = type.find('_');
    if (first == std::string_view::npos) {
        return std::string(type);
    }
    const auto second = type.find('_', first + 1);
    return std::string(type.substr(0, second));
}

template <typename T>
std::optional<Classified> bucket(const nlohmann::json& body,
                                 const char* source,
                                 std::optional<std::vector<T>> sbi_gen::DataNotification::*member) {
    try {
        Classified out;
        out.source = source;
        sbi_gen::DataNotification dn;
        (dn.*member) = std::vector<T>{body.get<T>()};
        out.notification.dataNotif = std::move(dn);
        return out;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt; // the header said AMF but the body is not an AmfEventNotification
    }
}

std::optional<Classified> analytics(const nlohmann::json& body) {
    try {
        Classified out;
        out.source = "NWDAF";
        out.notification.anaNotifications =
            std::vector<sbi_gen::NnwdafEventsSubscriptionNotification>{
                body.get<sbi_gen::NnwdafEventsSubscriptionNotification>()};
        return out;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::optional<Classified> by_api(const std::string& api, const nlohmann::json& body) {
    using D = sbi_gen::DataNotification;
    if (api == "Namf_EventExposure") {
        return bucket(body, "AMF", &D::amfEventNotifs);
    }
    if (api == "Nsmf_EventExposure") {
        return bucket(body, "SMF", &D::smfEventNotifs);
    }
    if (api == "Nudm_EE" || api == "Nudm_EventExposure") {
        return bucket(body, "UDM", &D::udmEventNotifs);
    }
    if (api == "Nnef_EventExposure") {
        return bucket(body, "NEF", &D::nefEventNotifs);
    }
    if (api == "Naf_EventExposure") {
        return bucket(body, "AF", &D::afEventNotifs);
    }
    if (api == "Nnrf_NFManagement") {
        return bucket(body, "NRF", &D::nrfEventNotifs);
    }
    if (api == "Nnsacf_SliceEventExposure") {
        return bucket(body, "NSACF", &D::nsacfEventNotifs);
    }
    if (api == "Nupf_EventExposure") {
        return bucket(body, "UPF", &D::upfEventNotifs);
    }
    if (api == "Ngmlc_Location") {
        return bucket(body, "GMLC", &D::gmlcEventNotifs);
    }
    if (api == "Nlmf_Location" || api == "Nlmf_DataExposure") {
        return bucket(body, "LMF", &D::lmfEventNotifs);
    }
    if (api == "Npcf_EventExposure") {
        return bucket(body, "PCF", &D::pcfEventNotifs);
    }
    if (api == "Nnwdaf_EventsSubscription") {
        return analytics(body);
    }
    return std::nullopt;
}

std::optional<Classified> by_shape(const nlohmann::json& body) {
    using D = sbi_gen::DataNotification;
    if (!body.is_object()) {
        return std::nullopt;
    }
    if (body.contains("subscriptionId") && body.contains("eventNotifications")) {
        return analytics(body);
    }
    if (body.contains("reportList") || body.contains("subsChangeNotifyCorrelationId")) {
        return bucket(body, "AMF", &D::amfEventNotifs);
    }
    if (body.contains("nfInstanceUri")) {
        return bucket(body, "NRF", &D::nrfEventNotifs);
    }
    if (body.contains("referenceId") && body.contains("eventType")) {
        return bucket(body, "UDM", &D::udmEventNotifs);
    }
    if (body.contains("report") && body.contains("notifyCorrelationId")) {
        return bucket(body, "NSACF", &D::nsacfEventNotifs);
    }
    if (body.contains("notificationItems")) {
        return bucket(body, "UPF", &D::upfEventNotifs);
    }
    if (body.contains("ldrReference") && body.contains("eventNotifyDataType")) {
        return bucket(body, "GMLC", &D::gmlcEventNotifs);
    }
    if (body.contains("reports") && body.contains("notifyCorrelationId")) {
        return bucket(body, "LMF", &D::lmfEventNotifs);
    }
    return std::nullopt;
}

} // namespace

std::optional<Classified> classify(std::optional<std::string_view> callback_header,
                                   const nlohmann::json& body) {
    if (callback_header) {
        if (auto c = by_api(api_name(*callback_header), body)) {
            return c;
        }
    }
    return by_shape(body);
}

} // namespace mfaf
