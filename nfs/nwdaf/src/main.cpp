// NWDAF -- Network Data Analytics Function, Phase A (ADR-0358) + Phase C (ADR-0368).
//
// Stage 2: TS 23.288 V19.7.0 (specs/3gpp/TS_23.288_j70.txt). Stage 3: the ten TS 29.520 R19
// YAML files in specs/5G_APIs-REL-19/ (forge.3gpp.org, REL-19, commit bca84b6). Phase A builds
// two of the ten services, both generated from their YAML and served at the API roots the YAML's
// own servers[0].url declares (ADR-0325 -- never derived from a filename):
//
//   Nnwdaf_AnalyticsInfo        {apiRoot}/nnwdaf-analyticsinfo/v1        GetNWDAFAnalytics,
//                                                                          GetNwdafContext
//   Nnwdaf_EventsSubscription   {apiRoot}/nnwdaf-eventssubscription/v1   the 7 operations on
//                                                                          /subscriptions and
//                                                                          /transfers
//   Nnwdaf_DataManagement       {apiRoot}/nnwdaf-datamanagement/v1       Subscribe (POST/PUT),
//                                                                          Unsubscribe, Notify,
//                                                                          Fetch (Phase C)
//
// Phase C (ADR-0368) is the data-collection half of TS 23.288. The AnLF collects the way 6.2.6.3.4
// describes: at startup it subscribes NRF NF-status data at the DCCF (Ndccf_DataManagement,
// nrfDataSub) with its own inbound URI {self}/nwdaf-inbound/v1/notifications/{source} as the
// notification target, and what the Messaging Framework delivers (the MFAF's
// NmfafDataRetrievalNotification, TS 29.576) is appended to a collected-events timeline in Valkey
// (collection_store.hpp) that every replica reads. NF_LOAD's NfStatus is then what TS 29.520
// defines it to be -- "the percentage of time spent on various NF states" -- time-weighted over
// the observation window, and its load the mean/peak of what the profiles reported
// (analytics.hpp nf_load). Nnwdaf_DataManagement (TS 29.520 4.4, TS 23.288 6.2.6.2) serves that
// collected data to other consumers -- an ADRF storing it, a DCCF, another NWDAF.
//
// Two analytics IDs are computed, each from the source TS 23.288 names for it -- see
// analytics.hpp. Every other NwdafEvent value the YAML defines is answered honestly: a request
// for an analytic this NWDAF does not compute gets the YAML's own 404 with
// ProblemDetailsAnalyticsInfoRequest semantics, not an empty 200 that looks like "no load".
//
// One binary, two logical functions (ADR-0359 #1): `role` in config selects anlf, mtlf or both.
// The MTLF (ADR-0369, mtlf.hpp) serves Nnwdaf_MLModelProvision and trains through the sidecar;
// the AnLF (ml_consumer.hpp) subscribes at the MTLF, retrieves the model from the ADRF and infers
// in-process with ONNX Runtime -- NF_LOAD requests for a future period are predictions (TS 23.288
// Table 6.5.3-2), past periods stay statistics. Nnwdaf_MLModelMonitor (ADR-0370): the AnLF
// registers the model it uses, serves the MTLF's accuracy subscription, judges its predictions
// against the loads then observed (accuracy_monitor.hpp) and notifies below threshold; the MTLF
// re-trains. Nnwdaf_MLModelTraining (federated learning between MTLFs),
// RoamingAnalytics/RoamingData/VFL are Phase D. State (subscriptions, transfers, collected data)
// lives in Valkey (ADR-0360), shared by every replica, and the notifier takes a per-subscription
// lease there (ADR-0365) so N replicas deliver each notification once.
//
// DISCLOSED, not hidden:
//   * ABNORMAL_BEHAVIOUR still reads the CHF feature store (feature_store.hpp). ADR-0359 planned
//     to retire that with Phase C; it is kept, because the spec's inputs for that analytic
//     (AMF/SMF/UPF UE events, 6.7.5.2) cannot flow through the DCCF today -- this AMF stores event
//     subscriptions but never fires them, UPF/SMF event exposure is not built -- and CHF usage is
//     the only real per-UE data in the lab. Recorded in ADR-0368 as a deviation, not hidden.
//   * Nnwdaf_DataManagement serves what this NWDAF collects: nrfDataSub, or an anaSub whose
//     events are NF_LOAD (the collected NRF data is that analytic's input). Anything else is
//     answered 400 SUBSCRIPTION_CANNOT_BE_SERVED (TS 29.520 4.4.2.2.2), never accepted unserved.
//     dataReports (summaries), muting / EnhDataMgmt, delAlert, immReport / DataAnaCollect,
//     terminationReq and the Nudm_SDM consent lookup are not built; per-UE requests without
//     checkedConsentInd are refused under the same consent policy knob as the DCCF's.
//   * ONE replica opens the collection at the DCCF (Valkey SET NX); the others read its timeline.
//     The DCCF delivers to that replica's self_base_url -- behind one service address that is
//     every replica; on a lab with per-replica ports it is the first one up. The collection is
//     not closed when a replica exits: it outlives replicas by design, and the DCCF keeps it.
//   * NF_LOAD lists an instance that left the NRF inside the observation window (its
//     unregistered share is the point); an instance never observed keeps the snapshot's 100%.
//   * /transfers is accepted and stored. No second NWDAF exists to transfer TO, so nothing is
//     transferred. The resource semantics (201/204/404) are real; the transfer is not.
//   * Notifications are produced by a periodic thread on the interval in config; the YAML's
//     NotificationMethod PERIODIC is honoured, THRESHOLD/ON_EVENT_DETECTION are treated as
//     periodic in Phase A.
//   * LI: TS 33.127 clause 7.18 requires an IRI-POI in NWDAF. Not built (blocker #0).

#include "sbi_core/datetime.hpp"
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/problem_details.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29520_Nnwdaf_VFLInference.hpp"
#include "TS29520_Nnwdaf_VFLTraining.hpp"
#include "accuracy_monitor.hpp"
#include "analytics.hpp"
#include "collection_store.hpp"
#include "feature_store.hpp"
#include "ml_consumer.hpp"
#include "ml_store.hpp"
#include "model_runtime.hpp"
#include "mtlf.hpp"
#include "nf_config/nf_config.hpp"
#include "nf_config/redis.hpp"
#include "qos_mon_collect.hpp"
#include "subscription_store.hpp"
#include "training_executor.hpp"
#include "vfl_subscription_store.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kNfType = "NWDAF";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";
// servers[0].url of each YAML, verified by tests/conformance's api_root_conformance (ADR-0325).
constexpr const char* kAnalyticsInfoRoot = "/nnwdaf-analyticsinfo/v1";
constexpr const char* kEventsSubscriptionRoot = "/nnwdaf-eventssubscription/v1";
constexpr const char* kDataManagementRoot = "/nnwdaf-datamanagement/v1";
// ADR-0370: the AnLF half of Nnwdaf_MLModelMonitor (subscriptions); the MTLF half
// (registrations) is in mtlf.cpp.
constexpr const char* kMonitorRoot = "/nnwdaf-mlmodelmonitor/v1";
// ADR-0380: the VFL (vertical federated learning) hook -- Nnwdaf_VFLTraining/Nnwdaf_VFLInference.
constexpr const char* kVflTrainingRoot = "/nnwdaf-vfltraining/v1";
constexpr const char* kVflInferenceRoot = "/nnwdaf-vflinference/v1";
constexpr const char* kMonitorCallback = "Nnwdaf_MLModelMonitor_myNotification";
// The NWDAF's own, non-3GPP URIs: where its collections deliver, and the fetchUri it hands out.
// Same convention as the MFAF's /mfaf-inbound/v1 and the ADRF's /adrf-inbound/v1.
constexpr const char* kInboundPrefix = "/nwdaf-inbound/v1";
// The DCCF's root -- servers[0].url of TS29574_Ndccf_DataManagement.yaml, used as a client.
constexpr const char* kDccfDataManagementRoot = "/ndccf-datamanagement/v1";
// 3gpp-Sbi-Callback values (TS 29.500 Annex B rule: <API>_<callback key>; both YAMLs key their
// callback "myNotification").
constexpr const char* kEventsCallback = "Nnwdaf_EventsSubscription_myNotification";
constexpr const char* kDataManagementCallback = "Nnwdaf_DataManagement_myNotification";
// The one data source Phase C collects: the NRF's NFStatusNotify stream.
constexpr const char* kNrfSource = "nrf";
// ADR-0379 slice 2: the collection timeline for the SMF Nsmf_EventExposure QOS_MON feed, which the
// SERVICE_EXPERIENCE analytic aggregates per S-NSSAI.
constexpr const char* kSmfQosMonSource = "smf_qos_mon";

// ADR-0380: state the energy-efficiency observable gauge's callback reads on each scrape. The
// callback is a captureless function pointer (OTel AddCallback takes void* state), so what it
// needs travels here: the collection store to read the QOS_MON window from, and the disclosed
// power model. Lives for the program's lifetime (a local in main outliving the meter).
struct EnergyEfficiencyGaugeState {
    nwdaf::CollectionStore* collected = nullptr;
    nwdaf::EnergyModel model;
};

std::optional<sbi_core::jwt::VerifyResult> check_bearer(const sbi_core::http2::Request& req,
                                                        sbi_core::jwt::Verifier& verifier) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    const std::string& value = it->second;
    constexpr std::string_view kPrefix = "Bearer ";
    if (value.size() <= kPrefix.size() || value.compare(0, kPrefix.size(), kPrefix) != 0) {
        sbi_core::jwt::VerifyResult r;
        r.valid = false;
        r.error = "Authorization header present but not a Bearer token";
        return r;
    }
    return verifier.verify(value.substr(kPrefix.size()));
}

// ADR-0380: the VFL hook's subscription CRUD, one instance per resource family. The lifecycle
// (create/retrieve/update/merge-patch/delete) is real and conformant to the TS 29.520 YAML; the
// federated-training coordination the subscription would drive (multi-party model exchange between
// MTLFs) is Phase D and not implemented -- disclosed. SubT is the subscription body DTO, PatchT its
// merge-patch DTO.
template <typename SubT, typename PatchT>
void register_vfl_crud(sbi_core::http2::Server& server,
                       const std::string& root,
                       nwdaf::VflSubscriptionStore& store,
                       sbi_core::jwt::Verifier& verifier) {
    const std::string collection = root + "/subscriptions";
    const std::string item = collection + "/{subscriptionId}";

    server.add_route(
        "POST", collection, [&store, &verifier, collection](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<SubT>(req, err);
            if (!body) {
                return err;
            }
            const auto id = store.create(nlohmann::json(*body));
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("location", collection + "/" + id);
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(*body).dump();
            return resp;
        });

    server.add_route("GET", item, [&store, &verifier](const sbi_core::http2::Request& req) {
        if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
            return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
        }
        const auto sub = store.get(req.path_params.at("subscriptionId"));
        if (!sub) {
            return sbi_core::http2::problem_response(404, "Not Found", "no such subscription");
        }
        sbi_core::http2::Response resp;
        resp.status = 200;
        resp.headers.emplace("content-type", "application/json");
        resp.body = sub->dump();
        return resp;
    });

    server.add_route("PUT", item, [&store, &verifier](const sbi_core::http2::Request& req) {
        if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
            return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
        }
        sbi_core::http2::Response err;
        auto body = sbi_core::http2::parse_json_body<SubT>(req, err);
        if (!body) {
            return err;
        }
        if (!store.replace(req.path_params.at("subscriptionId"), nlohmann::json(*body))) {
            return sbi_core::http2::problem_response(404, "Not Found", "no such subscription");
        }
        sbi_core::http2::Response resp;
        resp.status = 200;
        resp.headers.emplace("content-type", "application/json");
        resp.body = nlohmann::json(*body).dump();
        return resp;
    });

    server.add_route("PATCH", item, [&store, &verifier](const sbi_core::http2::Request& req) {
        if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
            return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
        }
        sbi_core::http2::Response err;
        auto patch = sbi_core::http2::parse_json_body<PatchT>(req, err);
        if (!patch) {
            return err;
        }
        const auto merged =
            store.merge_patch(req.path_params.at("subscriptionId"), nlohmann::json(*patch));
        if (!merged) {
            return sbi_core::http2::problem_response(404, "Not Found", "no such subscription");
        }
        sbi_core::http2::Response resp;
        resp.status = 200;
        resp.headers.emplace("content-type", "application/json");
        resp.body = merged->dump();
        return resp;
    });

    server.add_route("DELETE", item, [&store, &verifier](const sbi_core::http2::Request& req) {
        if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
            return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
        }
        if (!store.remove(req.path_params.at("subscriptionId"))) {
            return sbi_core::http2::problem_response(404, "Not Found", "no such subscription");
        }
        sbi_core::http2::Response resp;
        resp.status = 204;
        return resp;
    });
}

// A query parameter carrying a JSON object (TS 29.500 style for structured params). Absent is
// nullopt; present-but-malformed is an error the caller turns into the YAML's 400.
template <typename T>
std::optional<T>
json_query(const sbi_core::http2::Request& req, const char* name, std::string& err) {
    const auto it = req.query_params.find(name);
    if (it == req.query_params.end()) {
        return std::nullopt;
    }
    try {
        return json::parse(it->second).get<T>();
    } catch (const std::exception& e) {
        err = std::string("query parameter '") + name +
              "' is not valid JSON for its schema: " + e.what();
        return std::nullopt;
    }
}

// ---- data collection (TS 23.288 6.2) -----------------------------------------------------------

// NF_LOAD's input, straight from the NRF (Table 6.5.2-1). One discovery per NF type requested, or
// every type this project registers when the filter names none.
std::vector<sbi_gen::NFProfile_Nnrf_NFManagement>
collect_nf_profiles(sbi_core::http2::Client& client,
                    sbi_core::OAuth2Client& oauth,
                    const std::string& nrf_base,
                    const std::vector<std::string>& nf_types) {
    std::vector<sbi_gen::NFProfile_Nnrf_NFManagement> out;
    auto token = oauth.get_bearer_token();
    for (const auto& t : nf_types) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = nrf_base + "/nnrf-disc/v1/nf-instances?target-nf-type=" + t +
                  "&requester-nf-type=NWDAF";
        if (token) {
            req.headers.emplace("authorization", "Bearer " + *token);
        }
        auto resp = client.send(req);
        if (!resp || resp->status != 200) {
            spdlog::warn("nwdaf: NRF discovery for {} failed ({})", t, resp ? resp->status : -1);
            continue;
        }
        try {
            const auto body = json::parse(resp->body);
            for (const auto& p : body.at("nfInstances")) {
                out.push_back(p.get<sbi_gen::NFProfile_Nnrf_NFManagement>());
            }
        } catch (const std::exception& e) {
            spdlog::warn("nwdaf: NRF discovery for {} returned an unparseable SearchResult: {}",
                         t,
                         e.what());
        }
    }
    return out;
}

struct Analytics {
    // The set of NF types NF_LOAD covers when a request does not filter -- from config, so an
    // operator adds a type without a rebuild.
    std::vector<std::string> default_nf_types;
    nwdaf::AbnormalBehaviourThresholds thresholds;
};

// One collected NRF NotificationData (TS 29.510 6.1.6.2.x) as an NF-status observation: the
// event says registered / changed / deregistered, the profile (when carried) says the status and
// the load at that moment.
std::optional<nwdaf::NfStatusObservation> observation_from_nrf(const nwdaf::CollectedEvent& e) {
    if (!e.event.is_object() || !e.event.contains("event")) {
        return std::nullopt;
    }
    nwdaf::NfStatusObservation o;
    o.at = e.received_at;
    const std::string event = e.event.value("event", "");
    const json profile = e.event.value("nfProfile", json::object());
    if (profile.contains("nfInstanceId")) {
        o.nf_instance_id = profile.at("nfInstanceId").get<std::string>();
    } else if (e.event.contains("nfInstanceUri")) {
        const auto uri = e.event.at("nfInstanceUri").get<std::string>();
        o.nf_instance_id = uri.substr(uri.rfind('/') + 1);
    } else {
        return std::nullopt;
    }
    if (profile.contains("nfType")) {
        o.nf_type = profile.at("nfType").get<std::string>();
    }
    if (profile.contains("nfSetIdList") && profile.at("nfSetIdList").is_array() &&
        !profile.at("nfSetIdList").empty()) {
        o.nf_set_id = profile.at("nfSetIdList").front().get<std::string>();
    }
    if (profile.contains("load") && profile.at("load").is_number()) {
        o.load = profile.at("load").get<std::int64_t>();
    }
    if (event == "NF_DEREGISTERED") {
        o.status = "DEREGISTERED";
    } else {
        o.status = profile.value("nfStatus", "REGISTERED");
    }
    return o;
}

// The one function both Nnwdaf_AnalyticsInfo and the subscription notifier call, so a request
// and a notification for the same analytic cannot disagree.
std::optional<sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo>
compute(const std::string& event_id,
        const std::optional<sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo>& filter,
        const std::optional<sbi_gen::TargetUeInformation>& target,
        const std::optional<sbi_gen::EventReportingRequirement>& report_req,
        const Analytics& cfg,
        sbi_core::http2::Client& client,
        sbi_core::OAuth2Client& oauth,
        const std::string& nrf_base,
        nwdaf::FeatureStore& features,
        std::mutex& features_mutex,
        nwdaf::CollectionStore& collected,
        nwdaf::MlConsumer* ml) {
    sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo data;
    data.timeStampGen = sbi_core::format_rfc3339(std::chrono::system_clock::now());

    if (event_id == sbi_gen::NwdafEvent::NF_LOAD) {
        std::vector<std::string> types = cfg.default_nf_types;
        if (filter && filter->nfTypes && !filter->nfTypes->empty()) {
            types.clear();
            for (const auto& t : *filter->nfTypes) {
                types.push_back(t.value);
            }
        }
        auto profiles = collect_nf_profiles(client, oauth, nrf_base, types);
        if (filter && filter->nfInstanceIds && !filter->nfInstanceIds->empty()) {
            std::erase_if(profiles, [&](const auto& p) {
                return std::find(filter->nfInstanceIds->begin(),
                                 filter->nfInstanceIds->end(),
                                 p.nfInstanceId) == filter->nfInstanceIds->end();
            });
        }
        // Phase C: the snapshot plus every observation collected inside the window.
        const auto now = std::chrono::system_clock::now();
        const auto window_start = now - collected.window();
        std::vector<nwdaf::NfStatusObservation> observations;
        try {
            for (const auto& e : collected.events(kNrfSource, window_start, now)) {
                if (auto o = observation_from_nrf(e)) {
                    observations.push_back(std::move(*o));
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("nwdaf: collected NRF data unreadable, NF_LOAD from the snapshot only: {}",
                         e.what());
        }
        auto infos = nwdaf::nf_load(profiles, observations, window_start, now);
        // The request's filters apply to observed instances too, not only to the snapshot.
        std::erase_if(infos, [&](const sbi_gen::NfLoadLevelInformation& i) {
            if (filter && filter->nfInstanceIds && !filter->nfInstanceIds->empty() &&
                std::find(filter->nfInstanceIds->begin(),
                          filter->nfInstanceIds->end(),
                          i.nfInstanceId.value_or("")) == filter->nfInstanceIds->end()) {
                return true;
            }
            return i.nfType &&
                   std::find(types.begin(), types.end(), i.nfType->value) == types.end();
        });
        // TS 23.288 6.5.3: a target period in the future is a PREDICTION (Table 6.5.3-2, with
        // Confidence), computed with the MTLF-provisioned model over each instance's observed
        // load history (ADR-0369); a past period is statistics (Table 6.5.3-1). Without a model
        // or without enough history an instance keeps its statistics and carries no confidence
        // -- never a guess dressed as a prediction.
        std::optional<std::chrono::system_clock::time_point> end_ts;
        if (report_req && report_req->endTs) {
            end_ts = sbi_core::parse_rfc3339(*report_req->endTs);
        }
        if (ml && end_ts && *end_ts > now) {
            const auto model = ml->active_model(sbi_gen::NwdafEvent::NF_LOAD);
            for (auto& i : infos) {
                if (!model || !i.nfInstanceId) {
                    continue;
                }
                std::vector<nwdaf::LoadSample> history;
                for (const auto& o : observations) {
                    if (o.nf_instance_id == *i.nfInstanceId) {
                        history.push_back(nwdaf::LoadSample{o.load, o.status == "REGISTERED"});
                    }
                }
                const auto f = nwdaf::nf_load_features(history);
                if (!f) {
                    continue;
                }
                if (const auto predicted =
                        ml->predict(sbi_gen::NwdafEvent::NF_LOAD, *i.nfInstanceId, *f)) {
                    i.nfLoadLevelAverage = static_cast<std::int64_t>(std::lround(*predicted));
                    i.nfLoadLevelpeak.reset();
                    i.confidence = model->value("accuracyPct", std::int64_t(0));
                }
            }
            data.start = sbi_core::format_rfc3339(now);
            data.expiry = sbi_core::format_rfc3339(*end_ts);
        }
        data.nfLoadLevelInfos = std::move(infos);
        return data;
    }

    if (event_id == sbi_gen::NwdafEvent::ABNORMAL_BEHAVIOUR) {
        if (!features.connected()) {
            return std::nullopt; // the caller answers with the YAML's 404 + the real reason
        }
        std::vector<nwdaf::FeatureRow> today, yesterday;
        {
            const std::lock_guard<std::mutex> lock(features_mutex);
            const auto latest = features.latest_date();
            if (latest.empty()) {
                data.abnorBehavrs = std::vector<sbi_gen::AbnormalBehaviour>();
                return data; // a real, empty answer: no data yet
            }
            today = features.read_day(latest);
            // The day before, for the trend -- derived from the latest date, not from the clock.
            std::tm tm{};
            if (strptime(latest.c_str(), "%Y-%m-%d", &tm) != nullptr) {
                tm.tm_mday -= 1;
                const auto t = timegm(&tm);
                char buf[16];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d", gmtime(&t));
                yesterday = features.read_day(buf);
            }
        }
        if (target && target->supis && !target->supis->empty()) {
            const auto& keep = *target->supis;
            const auto not_targeted = [&](const nwdaf::FeatureRow& r) {
                return std::find(keep.begin(), keep.end(), r.subscriber_identifier) == keep.end();
            };
            std::erase_if(today, not_targeted);
            std::erase_if(yesterday, not_targeted);
        }
        data.abnorBehavrs = nwdaf::detect_abnormal_behaviour(today, yesterday, cfg.thresholds);
        return data;
    }

    if (event_id == sbi_gen::NwdafEvent::SERVICE_EXPERIENCE) {
        // TS 23.288 6.4: aggregate the collected SMF QOS_MON reports into a per-slice svcExprc.
        const auto now = std::chrono::system_clock::now();
        const auto window_start = now - collected.window();
        std::vector<json> reports;
        for (const auto& e : collected.events(kSmfQosMonSource, window_start, now)) {
            reports.push_back(e.event);
        }
        auto svc = nwdaf::service_experience(reports, std::nullopt);
        // Honour the request's slice filter verbatim (any SST/SD); an absent filter reports all.
        if (filter && filter->snssais && !filter->snssais->empty()) {
            std::vector<json> wanted;
            for (const auto& s : *filter->snssais) {
                wanted.push_back(json(s));
            }
            std::erase_if(svc, [&](const auto& info) {
                return !info.snssai ||
                       std::find(wanted.begin(), wanted.end(), json(*info.snssai)) == wanted.end();
            });
        }
        if (!svc.empty()) {
            data.svcExps = std::move(svc);
        }
        return data;
    }

    return std::nullopt; // an analytic this NWDAF does not compute
}

// ---- NRF lifecycle (same shape as every other NF, ADR-0006/0019) --------------------------------

void run_nrf_lifecycle(const std::string& instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4,
                       unsigned short port,
                       int heartbeat_seconds,
                       json nwdaf_info,
                       std::vector<std::string> service_names) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nwdaf/cert.pem",
        .key_path = CERTS_DIR "/nwdaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client http_client(std::move(client_tls));
    for (int attempt = 0; attempt < 300; ++attempt) {
        sbi_core::http2::ClientRequest probe;
        probe.method = "GET";
        probe.url = nrf_base + "/nnrf-nfm/v1/nf-instances/00000000-0000-4000-8000-000000000000";
        if (http_client.send(probe).has_value()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    sbi_core::OAuth2Client oauth(
        http_client, nrf_base + "/oauth2/token", instance_id, "nnrf-nfm", "NRF");
    json profile{
        {"nfInstanceId", instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({advertised_ipv4})},
        {"heartBeatTimer", heartbeat_seconds},
        // TS 29.510 NwdafInfo, per role (ADR-0359 #3): the AnLF's eventIds, the MTLF's
        // mlAnalyticsList -- a consumer discovering by analytics or by ML model finds the right
        // instance.
        {"nwdafInfo", std::move(nwdaf_info)},
    };
    // TS 29.510 NFService with an IpEndPoint per service, so a peer that discovers this instance
    // can address a service without configuration (ADR-0370: the MTLF finds an AnLF's
    // nnwdaf-mlmodelmonitor endpoint this way).
    json services = json::array();
    for (const auto& name : service_names) {
        services.push_back(json{
            {"serviceInstanceId", name},
            {"serviceName", name},
            {"versions",
             json::array({json{{"apiVersionInUri", "v1"}, {"apiFullVersion", "1.0.0"}}})},
            {"scheme", "https"},
            {"nfServiceStatus", "REGISTERED"},
            {"ipEndPoints",
             json::array(
                 {json{{"ipv4Address", advertised_ipv4}, {"transport", "TCP"}, {"port", port}}})}});
    }
    profile["nfServices"] = services;
    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token) {
            spdlog::error("nwdaf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("nwdaf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("nwdaf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(heartbeat_seconds / 2));
        auto token = oauth.get_bearer_token();
        if (!token) {
            continue;
        }
        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();
        // A dropped heartbeat lets the NRF deregister this NWDAF, so a failure is worth a line
        // rather than the silent fire-and-forget that tripped -Wunused-result on send()'s
        // [[nodiscard]] result.
        auto hb_resp = http_client.send(patch_req);
        if (!hb_resp) {
            spdlog::warn("nwdaf: NRF heartbeat failed: {}", hb_resp.error());
        } else if (hb_resp->status < 200 || hb_resp->status >= 300) {
            spdlog::warn("nwdaf: NRF heartbeat rejected (HTTP {})", hb_resp->status);
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("nwdaf");
    sbi_core::init_tracing("nwdaf");
    const auto config = nf_config::load("nwdaf", CONFIG_DIR);
    // Every port and tunable from config, env-overridable (user mandate). Nothing below is a
    // literal.
    const auto port = nf_config::require<unsigned short>(config, "port", "NWDAF_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "NWDAF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "NWDAF_NRF_BASE_URL");
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "NWDAF_ADVERTISED_IPV4");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "NWDAF_REDIS_URL");
    const auto heartbeat_seconds =
        nf_config::require<int>(config, "nrf_heartbeat_seconds", "NWDAF_NRF_HEARTBEAT_SECONDS");
    const auto notify_interval_seconds = nf_config::require<int>(
        config, "notification_interval_seconds", "NWDAF_NOTIFICATION_INTERVAL_SECONDS");
    // Phase C (ADR-0368): where this NWDAF is reachable for the DCCF's deliveries, where the DCCF
    // is, what to collect, and how much collected data to keep.
    const auto self_base =
        nf_config::require<std::string>(config, "self_base_url", "NWDAF_SELF_BASE_URL");
    // ADR-0379 slice 2: the SMF whose Nsmf_EventExposure QOS_MON feed this NWDAF collects. A
    // configured base URL rather than NRF discovery of the nsmf-event-exposure endpoint (disclosed
    // lab simplification -- a fuller impl discovers the producer via the NRF, as NF_LOAD does).
    const auto smf_base =
        nf_config::optional<std::string>(config, "smf_base_url", "NWDAF_SMF_BASE_URL");
    const auto dccf_base =
        nf_config::require<std::string>(config, "dccf_base_url", "NWDAF_DCCF_BASE_URL");
    const auto collection_cfg = config.at("data_collection");
    const auto collect_via_dccf =
        nf_config::require<bool>(collection_cfg, "via_dccf", "NWDAF_DATA_COLLECTION_VIA_DCCF");
    const auto collect_nrf_nf_types = nf_config::require<std::vector<std::string>>(
        collection_cfg, "nrf_status_nf_types", "NWDAF_DATA_COLLECTION_NRF_STATUS_NF_TYPES");
    const auto observation_window_seconds = nf_config::require<std::int64_t>(
        collection_cfg, "observation_window_seconds", "NWDAF_OBSERVATION_WINDOW_SECONDS");
    const auto max_collected_events = nf_config::require<std::int64_t>(
        collection_cfg, "max_collected_events", "NWDAF_MAX_COLLECTED_EVENTS");
    const auto collector_heartbeat_seconds = nf_config::require<std::int64_t>(
        collection_cfg, "holder_heartbeat_seconds", "NWDAF_COLLECTION_HOLDER_HEARTBEAT_SECONDS");
    const auto fetch_ttl = nf_config::require<std::int64_t>(
        config, "fetch_buffer_ttl_seconds", "NWDAF_FETCH_BUFFER_TTL_SECONDS");
    const auto consent_policy =
        nf_config::require<std::string>(config, "user_consent_policy", "NWDAF_USER_CONSENT_POLICY");
    // ADR-0369: the logical function(s) this instance runs, and the MTLF/AnLF settings.
    const auto role = nf_config::require<std::string>(config, "role", "NWDAF_ROLE");
    if (role != "anlf" && role != "mtlf" && role != "both") {
        spdlog::critical("nwdaf: role must be \"anlf\", \"mtlf\" or \"both\", got \"{}\"", role);
        return 1;
    }
    const bool is_anlf = role != "mtlf";
    const bool is_mtlf = role != "anlf";
    const auto mtlf_base =
        nf_config::require<std::string>(config, "mtlf_base_url", "NWDAF_MTLF_BASE_URL");
    const auto adrf_base =
        nf_config::require<std::string>(config, "adrf_base_url", "NWDAF_ADRF_BASE_URL");
    const auto ml_cfg = config.at("ml_model_provision");
    nwdaf::MlConsumerOptions ml_opts;
    ml_opts.mtlf_base_url = mtlf_base;
    ml_opts.notif_uri = self_base + kInboundPrefix + "/ml-models";
    ml_opts.events = nf_config::require<std::vector<std::string>>(
        ml_cfg, "events", "NWDAF_ML_MODEL_PROVISION_EVENTS");
    ml_opts.nf_types = nf_config::require<std::vector<std::string>>(
        ml_cfg, "nf_types", "NWDAF_ML_MODEL_PROVISION_NF_TYPES");
    ml_opts.accuracy_threshold = nf_config::require<std::int64_t>(
        ml_cfg, "accuracy_threshold", "NWDAF_ML_MODEL_PROVISION_ACCURACY_THRESHOLD");
    ml_opts.holder_heartbeat_seconds = nf_config::require<std::int64_t>(
        ml_cfg, "holder_heartbeat_seconds", "NWDAF_ML_MODEL_PROVISION_HOLDER_HEARTBEAT_SECONDS");
    ml_opts.retry_seconds = nf_config::require<std::int64_t>(
        ml_cfg, "retry_seconds", "NWDAF_ML_MODEL_PROVISION_RETRY_SECONDS");
    const auto mtlf_cfg = config.at("mtlf");
    nwdaf::MtlfOptions mtlf_opts;
    mtlf_opts.nrf_base = nrf_base;
    mtlf_opts.adrf_base_url = adrf_base;
    mtlf_opts.events =
        nf_config::require<std::vector<std::string>>(mtlf_cfg, "events", "NWDAF_MTLF_EVENTS");
    mtlf_opts.data_set_id =
        nf_config::require<std::string>(mtlf_cfg, "data_set_id", "NWDAF_MTLF_DATA_SET_ID");
    mtlf_opts.min_samples =
        nf_config::require<std::int64_t>(mtlf_cfg, "min_samples", "NWDAF_MTLF_MIN_SAMPLES");
    mtlf_opts.accuracy_tolerance =
        nf_config::require<double>(mtlf_cfg, "accuracy_tolerance", "NWDAF_MTLF_ACCURACY_TOLERANCE");
    mtlf_opts.check_interval_seconds = nf_config::require<std::int64_t>(
        mtlf_cfg, "check_interval_seconds", "NWDAF_MTLF_CHECK_INTERVAL_SECONDS");
    mtlf_opts.retrain_min_new_windows = nf_config::require<std::int64_t>(
        mtlf_cfg, "retrain_min_new_windows", "NWDAF_MTLF_RETRAIN_MIN_NEW_WINDOWS");
    mtlf_opts.retrain_interval_seconds = nf_config::require<std::int64_t>(
        mtlf_cfg, "retrain_interval_seconds", "NWDAF_MTLF_RETRAIN_INTERVAL_SECONDS");
    mtlf_opts.training_lease_seconds = nf_config::require<std::int64_t>(
        mtlf_cfg, "training_lease_seconds", "NWDAF_MTLF_TRAINING_LEASE_SECONDS");
    const auto training_cfg = config.at("training");
    nwdaf::SubprocessExecutorOptions train_opts;
    train_opts.python =
        nf_config::require<std::string>(training_cfg, "python", "NWDAF_TRAINING_PYTHON");
    train_opts.script =
        nf_config::require<std::string>(training_cfg, "script", "NWDAF_TRAINING_SCRIPT");
    train_opts.workdir =
        nf_config::require<std::string>(training_cfg, "workdir", "NWDAF_TRAINING_WORKDIR");
    train_opts.mlflow_tracking_uri = nf_config::require<std::string>(
        training_cfg, "mlflow_tracking_uri", "NWDAF_TRAINING_MLFLOW_TRACKING_URI");
    train_opts.timeout = std::chrono::seconds(nf_config::require<std::int64_t>(
        training_cfg, "timeout_seconds", "NWDAF_TRAINING_TIMEOUT_SECONDS"));
    // Empty training paths mean "this build's own nfs/nwdaf/training" (the sidecar's venv, its
    // script, a runs/ directory beside it, and MLflow's SQLite file in that directory). A
    // deployment sets them explicitly (the compose service does).
    if (train_opts.python.empty()) {
        train_opts.python = NWDAF_TRAINING_DIR "/.venv/bin/python3";
    }
    if (train_opts.script.empty()) {
        train_opts.script = NWDAF_TRAINING_DIR "/train_nf_load.py";
    }
    if (train_opts.workdir.empty()) {
        train_opts.workdir = NWDAF_TRAINING_DIR "/runs";
    }
    if (train_opts.mlflow_tracking_uri.empty()) {
        train_opts.mlflow_tracking_uri = "sqlite:///" + train_opts.workdir + "/mlflow.db";
    }
    mtlf_opts.self_base = self_base;
    mtlf_opts.accuracy_threshold = nf_config::require<std::int64_t>(
        mtlf_cfg, "accuracy_threshold", "NWDAF_MTLF_ACCURACY_THRESHOLD");
    mtlf_opts.retrain_cooldown_seconds = nf_config::require<std::int64_t>(
        mtlf_cfg, "retrain_cooldown_seconds", "NWDAF_MTLF_RETRAIN_COOLDOWN_SECONDS");
    ml_opts.nrf_base = nrf_base;
    // ADR-0370: the AnLF's accuracy monitoring of the models it uses.
    const auto acc_cfg = config.at("accuracy_monitoring");
    const auto acc_tolerance =
        nf_config::require<double>(acc_cfg, "tolerance", "NWDAF_ACCURACY_TOLERANCE");
    const auto acc_window_seconds = nf_config::require<std::int64_t>(
        acc_cfg, "window_seconds", "NWDAF_ACCURACY_WINDOW_SECONDS");
    const auto acc_truth_timeout_seconds = nf_config::require<std::int64_t>(
        acc_cfg, "truth_timeout_seconds", "NWDAF_ACCURACY_TRUTH_TIMEOUT_SECONDS");
    const auto acc_check_interval_seconds = nf_config::require<std::int64_t>(
        acc_cfg, "check_interval_seconds", "NWDAF_ACCURACY_CHECK_INTERVAL_SECONDS");
    const auto acc_min_inferences = nf_config::require<std::int64_t>(
        acc_cfg, "min_inferences", "NWDAF_ACCURACY_MIN_INFERENCES");
    const auto acc_default_threshold = nf_config::require<std::int64_t>(
        acc_cfg, "default_threshold", "NWDAF_ACCURACY_DEFAULT_THRESHOLD");
    const auto acc_min_report_interval_seconds = nf_config::require<std::int64_t>(
        acc_cfg, "min_report_interval_seconds", "NWDAF_ACCURACY_MIN_REPORT_INTERVAL_SECONDS");
    if (is_mtlf && adrf_base.empty()) {
        spdlog::critical("nwdaf: role {} needs adrf_base_url (the MTLF stores models and reads "
                         "training data through the ADRF, ADR-0369)",
                         role);
        return 1;
    }
    if (consent_policy != "consumer-checked" && consent_policy != "not-enforced") {
        spdlog::critical("nwdaf: user_consent_policy must be \"consumer-checked\" or "
                         "\"not-enforced\", got \"{}\"",
                         consent_policy);
        return 1;
    }

    Analytics analytics;
    analytics.default_nf_types =
        nf_config::require<std::vector<std::string>>(config, "nf_load_default_nf_types");
    analytics.thresholds.sigma_threshold = nf_config::require<double>(
        config, "abnormal_sigma_threshold", "NWDAF_ABNORMAL_SIGMA_THRESHOLD");
    analytics.thresholds.large_rate_floor_octets = nf_config::require<double>(
        config, "abnormal_large_rate_floor_octets", "NWDAF_ABNORMAL_LARGE_RATE_FLOOR_OCTETS");
    analytics.thresholds.session_count_threshold = nf_config::require<std::int64_t>(
        config, "abnormal_session_count_threshold", "NWDAF_ABNORMAL_SESSION_COUNT_THRESHOLD");
    analytics.thresholds.max_exception_level =
        nf_config::require<std::int64_t>(config, "abnormal_max_exception_level");
    analytics.thresholds.confidence_full_at_sessions =
        nf_config::require<std::int64_t>(config, "abnormal_confidence_full_at_sessions");

    // ADR-0380: the energy-efficiency KPI's disclosed power model (a TS 28.552 PEEC stand-in --
    // the 5G SBI carries no energy telemetry). Both terms come from config, never a source literal.
    nwdaf::EnergyModel energy_model;
    energy_model.static_watts_per_slice = nf_config::require<double>(
        config, "energy_static_watts_per_slice", "NWDAF_ENERGY_STATIC_WATTS_PER_SLICE");
    energy_model.joules_per_gigabyte = nf_config::require<double>(
        config, "energy_joules_per_gigabyte", "NWDAF_ENERGY_JOULES_PER_GIGABYTE");

    nwdaf::FeatureStoreOptions fs;
    fs.host =
        nf_config::require<std::string>(config, "feature_store_host", "NWDAF_FEATURE_STORE_HOST");
    fs.port =
        nf_config::require<std::uint16_t>(config, "feature_store_port", "NWDAF_FEATURE_STORE_PORT");
    fs.user =
        nf_config::require<std::string>(config, "feature_store_user", "NWDAF_FEATURE_STORE_USER");
    fs.password = nf_config::require<std::string>(
        config, "feature_store_password", "NWDAF_FEATURE_STORE_PASSWORD");
    fs.database = nf_config::require<std::string>(
        config, "feature_store_database", "NWDAF_FEATURE_STORE_DATABASE");
    fs.max_rows = nf_config::require<std::int64_t>(
        config, "feature_store_max_rows", "NWDAF_FEATURE_STORE_MAX_ROWS");

    sbi_core::init_metrics(metrics_bind_address);
    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("nwdaf: starting, nfInstanceId={}, role={}", instance_id, role);
    ml_opts.instance_id = instance_id;
    mtlf_opts.instance_id = instance_id;

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/nwdaf/cert.pem",
        .key_path = CERTS_DIR "/nwdaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    // Outbound: NRF discovery for NF_LOAD, and notification delivery.
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nwdaf/cert.pem",
        .key_path = CERTS_DIR "/nwdaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client client(std::move(client_tls));
    sbi_core::OAuth2Client oauth(
        client, nrf_base + "/oauth2/token", instance_id, "nnrf-disc", "NRF");
    sbi_core::OAuth2Client oauth_dccf(
        client, nrf_base + "/oauth2/token", instance_id, "ndccf-datamanagement", "DCCF");
    sbi_core::OAuth2Client oauth_smf(
        client, nrf_base + "/oauth2/token", instance_id, "nsmf-event-exposure", "SMF");
    sbi_core::OAuth2Client oauth_adrf_dm(
        client, nrf_base + "/oauth2/token", instance_id, "nadrf-datamanagement", "ADRF");
    sbi_core::OAuth2Client oauth_adrf_ml(
        client, nrf_base + "/oauth2/token", instance_id, "nadrf-mlmodelmanagement", "ADRF");
    std::mutex client_mutex; // the client is used from the request path and the notifier thread

    nwdaf::FeatureStore features(fs);
    // Architecture-bonded fail-fast: the feature store (Doris) is a persistence dependency; if it
    // is unreachable at startup the process must exit, not run serving analytics degraded.
    if (!features.connected()) {
        nf_config::fatal("nwdaf: feature store (Doris) is unreachable at startup");
    }
    std::mutex features_mutex;
    // ADR-0360: subscription state in Valkey, shared by every NWDAF replica; ADR-0368: the
    // collected data and the data-management subscriptions too.
    auto redis = nf_config::connect_redis_or_die(redis_url, "nwdaf");
    nwdaf::SubscriptionStore store(redis);
    nwdaf::CollectionStore collected(
        redis, std::chrono::seconds(observation_window_seconds), max_collected_events);
    // ADR-0369: ML-model state (both roles), the AnLF's model consumer, the MTLF.
    nwdaf::MlStore ml_store(redis);
    // The consumer gets its own client: predict() runs inside compute(), which the request path
    // and the notifier call under client_mutex -- fetching the model bytes through the shared
    // client from there would deadlock on that mutex.
    sbi_core::http2::TlsConfig ml_client_tls{
        .cert_path = CERTS_DIR "/nwdaf/cert.pem",
        .key_path = CERTS_DIR "/nwdaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client ml_client(std::move(ml_client_tls));
    std::mutex ml_client_mutex;
    sbi_core::OAuth2Client oauth_mtlf(
        ml_client, nrf_base + "/oauth2/token", instance_id, "nnwdaf-mlmodelprovision", "NWDAF");
    sbi_core::OAuth2Client oauth_anlf_adrf_ml(
        ml_client, nrf_base + "/oauth2/token", instance_id, "nadrf-mlmodelmanagement", "ADRF");
    nwdaf::MlConsumer ml_consumer(
        ml_opts, ml_client, ml_client_mutex, oauth_mtlf, oauth_anlf_adrf_ml, ml_store);
    nwdaf::AccuracyMonitor accuracy(redis,
                                    acc_tolerance,
                                    std::chrono::seconds(acc_window_seconds),
                                    std::chrono::seconds(acc_truth_timeout_seconds));
    ml_consumer.attach_monitor(&accuracy);
    nwdaf::MlConsumer* ml = (is_anlf && ml_consumer.enabled()) ? &ml_consumer : nullptr;
    nwdaf::SubprocessExecutor trainer(train_opts);
    nwdaf::Mtlf mtlf(mtlf_opts,
                     client,
                     client_mutex,
                     oauth,
                     oauth_adrf_dm,
                     oauth_adrf_ml,
                     verifier,
                     ml_store,
                     trainer);

    auto meter = sbi_core::get_meter("nwdaf");
    auto analytics_counter = meter->CreateUInt64Counter("nwdaf_analytics_requests_total",
                                                        "Nnwdaf_AnalyticsInfo requests served");
    auto unsupported_counter =
        meter->CreateUInt64Counter("nwdaf_analytics_unsupported_total",
                                   "Requests for an analytics ID this NWDAF does not compute");
    auto notify_counter = meter->CreateUInt64Counter(
        "nwdaf_notifications_sent_total", "Nnwdaf_EventsSubscription notifications delivered");
    auto collected_counter = meter->CreateUInt64Counter(
        "nwdaf_collected_events_total", "Data-source events collected through the DCCF");
    auto dm_notify_counter =
        meter->CreateUInt64Counter("nwdaf_datamanagement_notifications_total",
                                   "Nnwdaf_DataManagement notifications delivered");
    auto dm_rejected_counter = meter->CreateUInt64Counter(
        "nwdaf_datamanagement_rejected_total",
        "Nnwdaf_DataManagement subscriptions answered SUBSCRIPTION_CANNOT_BE_SERVED / "
        "USER_CONSENT_NOT_GRANTED");

    // ADR-0380: energy-efficiency analytics. There is no Nnwdaf energy eventId (energy is OAM/TS
    // 28.552-sourced), so the R19-faithful surface is an OAM-style metric, not an analytics
    // response: a per-S-NSSAI TS 28.554 6.7.1 Energy-Efficiency KPI (data volume / energy) over the
    // collected SMF QOS_MON window, with energy from the disclosed power model. Observed on each
    // scrape, one point per slice labelled with its verbatim S-NSSAI (any SST/SD).
    EnergyEfficiencyGaugeState ee_state{&collected, energy_model};
    auto energy_efficiency_gauge = meter->CreateDoubleObservableGauge(
        "nwdaf_slice_energy_efficiency_bit_per_joule",
        "TS 28.554 6.7.1 slice energy efficiency (data volume / energy consumption); energy from a "
        "disclosed TS 28.552 PEEC stand-in power model");
    energy_efficiency_gauge->AddCallback(
        [](opentelemetry::metrics::ObserverResult observer_result, void* state) {
            auto* st = static_cast<EnergyEfficiencyGaugeState*>(state);
            const auto now = std::chrono::system_clock::now();
            const auto window = st->collected->window();
            const auto window_start = now - window;
            std::vector<json> reports;
            for (const auto& e : st->collected->events(kSmfQosMonSource, window_start, now)) {
                reports.push_back(e.event);
            }
            const double window_seconds =
                std::chrono::duration_cast<std::chrono::duration<double>>(window).count();
            const auto ee =
                nwdaf::energy_efficiency(reports, std::nullopt, st->model, window_seconds);
            if (auto obs = opentelemetry::nostd::get_if<opentelemetry::nostd::shared_ptr<
                    opentelemetry::metrics::ObserverResultT<double>>>(&observer_result)) {
                for (const auto& slice : ee) {
                    (*obs)->Observe(slice.efficiency_bit_per_joule,
                                    {{"snssai", slice.snssai.dump()}});
                }
            }
        },
        &ee_state);

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);

    // ADR-0380: the VFL hook -- Nnwdaf_VFLTraining/VFLInference subscription CRUD. The lifecycle is
    // real; the federated-training coordination the subscriptions would drive is Phase D
    // (disclosed).
    nwdaf::VflSubscriptionStore vfl_training_store("nwdaf-vflt-");
    nwdaf::VflSubscriptionStore vfl_inference_store("nwdaf-vfli-");
    register_vfl_crud<sbi_gen::VflTrainingSubs_Nnwdaf_VFLTraining,
                      sbi_gen::VflTrainingSubsPatch_Nnwdaf_VFLTraining>(
        server, kVflTrainingRoot, vfl_training_store, verifier);
    register_vfl_crud<sbi_gen::VflInferSub_Nnwdaf_VFLInference,
                      sbi_gen::VflInferSubPatch_Nnwdaf_VFLInference>(
        server, kVflInferenceRoot, vfl_inference_store, verifier);

    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
    }

    // ---- Nnwdaf_AnalyticsInfo ----------------------------------------------------------------
    server.add_route(
        "GET",
        std::string(kAnalyticsInfoRoot) + "/analytics",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ev = req.query_params.find("event-id");
            if (ev == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "query parameter 'event-id' is required");
            }
            std::string err;
            auto filter =
                json_query<sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo>(req, "event-filter", err);
            if (!err.empty()) {
                return sbi_core::http2::problem_response(400, "Bad Request", err);
            }
            auto target = json_query<sbi_gen::TargetUeInformation>(req, "tgt-ue", err);
            if (!err.empty()) {
                return sbi_core::http2::problem_response(400, "Bad Request", err);
            }
            auto ana_req = json_query<sbi_gen::EventReportingRequirement>(req, "ana-req", err);
            if (!err.empty()) {
                return sbi_core::http2::problem_response(400, "Bad Request", err);
            }
            std::optional<sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo> data;
            {
                const std::lock_guard<std::mutex> lock(client_mutex);
                data = compute(ev->second,
                               filter,
                               target,
                               ana_req,
                               analytics,
                               client,
                               oauth,
                               nrf_base,
                               features,
                               features_mutex,
                               collected,
                               ml);
            }
            if (!data) {
                unsupported_counter->Add(1);
                // The YAML's 404 for this operation. The detail says WHICH of the two reasons.
                const std::string why =
                    ev->second == sbi_gen::NwdafEvent::ABNORMAL_BEHAVIOUR
                        ? "ABNORMAL_BEHAVIOUR needs the CHF feature store, which is unreachable"
                        : "analytics ID '" + ev->second +
                              "' is not computed by this NWDAF (NF_LOAD, "
                              "ABNORMAL_BEHAVIOUR, SERVICE_EXPERIENCE)";
                return sbi_core::http2::problem_response(404, "Not Found", why);
            }
            analytics_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(*data).dump();
            return resp;
        });

    server.add_route("GET",
                     std::string(kAnalyticsInfoRoot) + "/context",
                     [&](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                             return sbi_core::http2::problem_response(
                                 401, "Unauthorized", auth->error);
                         }
                         // GetNwdafContext returns ContextData for the requested context-ids. Phase
                         // A holds no per-UE context (no DataManagement yet), so the YAML's 204 "no
                         // content" is the true answer rather than an empty object pretending to be
                         // context.
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });

    // ---- Nnwdaf_EventsSubscription -----------------------------------------------------------
    server.add_route(
        "POST",
        std::string(kEventsSubscriptionRoot) + "/subscriptions",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::NnwdafEventsSubscription>(req, err);
            if (!body) {
                return err;
            }
            if (body->eventSubscriptions.empty()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "eventSubscriptions must not be empty");
            }
            const auto id = store.create_subscription(*body);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEventsSubscriptionRoot) + "/subscriptions/" + id);
            resp.body = json(*body).dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kEventsSubscriptionRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::NnwdafEventsSubscription>(req, err);
            if (!body) {
                return err;
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!store.replace_subscription(id, *body)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no subscription " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(*body).dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kEventsSubscriptionRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!store.remove_subscription(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no subscription " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // /transfers -- stored, not transferred (see the header). Real resource semantics.
    server.add_route(
        "POST",
        std::string(kEventsSubscriptionRoot) + "/transfers",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AnalyticsSubscriptionsTransfer>(req, err);
            if (!body) {
                return err;
            }
            const auto id = store.create_transfer(*body);
            spdlog::warn("nwdaf: analytics subscription transfer {} accepted and stored; no peer "
                         "NWDAF exists to transfer to (Phase A disclosure)",
                         id);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("location",
                                 std::string(kEventsSubscriptionRoot) + "/transfers/" + id);
            return resp;
        });
    server.add_route(
        "PUT",
        std::string(kEventsSubscriptionRoot) + "/transfers/{transferId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AnalyticsSubscriptionsTransfer>(req, err);
            if (!body) {
                return err;
            }
            const auto id = req.path_params.at("transferId");
            if (!store.replace_transfer(id, *body)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no transfer " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    server.add_route(
        "DELETE",
        std::string(kEventsSubscriptionRoot) + "/transfers/{transferId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("transferId");
            if (!store.remove_transfer(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no transfer " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ============ Phase C: data collection via the DCCF, and Nnwdaf_DataManagement ============

    const auto problem =
        [](int status, const std::string& title, const std::string& detail, const char* cause) {
            auto pd = sbi_core::make_problem_details(status, title, detail, cause);
            sbi_core::http2::Response r;
            r.status = status;
            r.headers.emplace("content-type", "application/problem+json");
            r.body = json(pd).dump();
            return r;
        };

    // Nnwdaf_DataManagement_Notify: one NnwdafDataManagementNotif per subscription, carrying the
    // collected NRF events as a DataNotification -- or, under consTrigNotif, a fetch instruction.
    const auto dm_notify = [&](const std::string& sub_id,
                               const nwdaf::DataManagementSubscription& sub,
                               const std::vector<nwdaf::CollectedEvent>& events) {
        if (events.empty()) {
            return;
        }
        json nrf_notifs = json::array();
        for (const auto& e : events) {
            nrf_notifs.push_back(e.event);
        }
        const auto now = std::chrono::system_clock::now();
        json note{
            {"notifCorrId", sub.request.at("notifCorrId")},
            {"notifTimestamp", sbi_core::format_rfc3339(now)},
            {"dataNotification",
             json{{"nrfEventNotifs", nrf_notifs}, {"timeStamp", sbi_core::format_rfc3339(now)}}}};
        const bool fetch = sub.request.contains("formatInstruct") &&
                           sub.request.at("formatInstruct").value("consTrigNotif", false);
        if (fetch) {
            const auto fetch_id = collected.next_id("nwdaf-fetch-");
            collected.put_fetch(fetch_id, note, std::chrono::seconds(fetch_ttl));
            note = json{{"notifCorrId", sub.request.at("notifCorrId")},
                        {"notifTimestamp", sbi_core::format_rfc3339(now)},
                        {"fetchInstruct",
                         json{{"fetchUri", self_base + kInboundPrefix + "/fetch"},
                              {"fetchCorrIds", json::array({fetch_id})},
                              {"expiry",
                               sbi_core::format_rfc3339(now + std::chrono::seconds(fetch_ttl))}}}};
        }
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = sub.request.at("notificURI").get<std::string>();
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace(sbi_core::headers::kCallback, kDataManagementCallback);
        req.body = note.dump();
        std::optional<int> status;
        {
            const std::lock_guard<std::mutex> lock(client_mutex);
            if (auto resp = client.send(req); resp) {
                status = static_cast<int>(resp->status);
            }
        }
        if (status && *status >= 200 && *status < 300) {
            dm_notify_counter->Add(1);
        } else {
            spdlog::warn("nwdaf: Nnwdaf_DataManagement_Notify for {} to {} failed ({})",
                         sub_id,
                         req.url,
                         status.value_or(-1));
        }
    };

    // ---- inbound: what the Messaging Framework delivers for a collection --------------------
    server.add_route(
        "POST",
        std::string(kInboundPrefix) + "/notifications/{source}",
        [&](const sbi_core::http2::Request& req) {
            const auto source = req.path_params.at("source");
            if (source != kNrfSource) {
                return problem(400,
                               "Bad Request",
                               "no NWDAF collection for source " + source,
                               "RESOURCE_CONTEXT_NOT_FOUND");
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            // NmfafDataRetrievalNotification (TS 29.576): dataAnaNotif.dataNotif.nrfEventNotifs.
            const json dan = body.value("dataAnaNotif", json::object());
            const json dn = dan.value("dataNotif", json::object());
            if (!dn.contains("nrfEventNotifs")) {
                if (body.contains("fetchInstruction")) {
                    spdlog::warn("nwdaf: the MFAF delivered a fetch instruction; this NWDAF "
                                 "subscribes without consTrigNotif -- ignored");
                }
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            std::vector<nwdaf::CollectedEvent> fresh;
            try {
                for (const auto& n : dn.at("nrfEventNotifs")) {
                    fresh.push_back(collected.append(kNrfSource, n));
                }
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
            collected_counter->Add(fresh.size());
            spdlog::info("nwdaf: collected {} NRF event(s) via the DCCF", fresh.size());
            // Every Nnwdaf_DataManagement subscription served from this source gets them now
            // (TS 23.288 6.2.6.2 step 6b).
            for (const auto& [id, sub] : collected.all_dm_subscriptions()) {
                if (sub.source == kNrfSource) {
                    dm_notify(id, sub, fresh);
                }
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- inbound: the SMF's Nsmf_EventExposure QOS_MON reports (ADR-0379 slice 2) -----------
    // The SMF POSTs NsmfEventExposureNotifications here (the notifUri of the subscription this
    // NWDAF creates at startup). Each QOS_MON EventNotification (snssai + ul/dl/rtDelays) is
    // appended to the collection timeline verbatim -- any S-NSSAI, standard or custom -- for the
    // SERVICE_EXPERIENCE analytic (slice 3) to aggregate per slice.
    server.add_route(
        "POST", std::string(kInboundPrefix) + "/qos-mon", [&](const sbi_core::http2::Request& req) {
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto reports = nwdaf::qos_mon_event_notifs(body);
            for (const auto& report : reports) {
                collected.append(kSmfQosMonSource, report);
            }
            collected_counter->Add(reports.size());
            if (!reports.empty()) {
                spdlog::info("nwdaf: collected {} QOS_MON report(s) from the SMF", reports.size());
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- inbound: Nnwdaf_MLModelProvision_Notify from the MTLF (ADR-0369) -------------------
    if (ml) {
        server.add_route("POST",
                         std::string(kInboundPrefix) + "/ml-models",
                         [&](const sbi_core::http2::Request& req) {
                             json body;
                             try {
                                 body = json::parse(req.body);
                             } catch (const json::parse_error& e) {
                                 return sbi_core::http2::problem_response(
                                     400, "Malformed JSON", e.what());
                             }
                             try {
                                 ml_consumer.on_notification(body);
                             } catch (const std::exception& e) {
                                 return sbi_core::http2::problem_response(
                                     500, "Internal Server Error", e.what());
                             }
                             sbi_core::http2::Response resp;
                             resp.status = 204;
                             return resp;
                         });
    }
    if (is_mtlf) {
        mtlf.install_routes(server);
    }

    // ---- Nnwdaf_MLModelMonitor_Subscribe / _Unsubscribe, served by the AnLF (ADR-0370) ------
    // TS 29.520 4.7.2.4-4.7.2.5: the MTLF subscribes here for the accuracy of the models this
    // AnLF uses. A report (MLModelMonitorNotify) for the models a subscription names, now.
    const auto monitor_report = [&](const json& sub) {
        json infos = json::array();
        bool meets = true;
        bool any = false;
        const auto threshold = sub.value("accuThreshold", acc_default_threshold);
        for (const auto& mid : sub.value("modelIds", json::array())) {
            const auto id = mid.get<std::int64_t>();
            const auto sum = accuracy.summary(sbi_gen::NwdafEvent::NF_LOAD, id);
            if (sum.inferences < acc_min_inferences) {
                continue;
            }
            any = true;
            meets = meets && sum.accuracy_pct() >= threshold;
            infos.push_back(json{
                {"modelId", id},
                {"modelMetric", sbi_gen::MLModelMetric::ACCURACY},
                {"mlModelAcc", sum.accuracy_pct()},
                {"inferenceNum", sum.inferences},
                {"deviation", sum.mean_abs_deviation},
                {"monitorInterval",
                 json{{"startTime",
                       sbi_core::format_rfc3339(std::chrono::system_clock::now() -
                                                std::chrono::seconds(acc_window_seconds))},
                      {"stopTime", sbi_core::format_rfc3339(std::chrono::system_clock::now())}}}});
        }
        if (!any) {
            return std::optional<json>();
        }
        json note{{"notifCorrId", sub.value("notifCorrId", "")},
                  {"modelAccuInfos", infos},
                  {"accuMeetInd", meets},
                  {"mLEvent", sbi_gen::NwdafEvent::NF_LOAD}};
        return std::optional<json>(note);
    };
    const auto monitor_validate = [&](const sbi_core::http2::Request& req,
                                      json& record,
                                      std::optional<sbi_core::http2::Response>& err) {
        if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
            err = sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            return;
        }
        sbi_core::http2::Response bad;
        auto dto = sbi_core::http2::parse_json_body<sbi_gen::MLModelMonitorSub>(req, bad);
        if (!dto) {
            err = bad;
            return;
        }
        if (dto->modelIds.empty()) {
            err =
                problem(400, "Bad Request", "modelIds shall not be empty", "MANDATORY_IE_MISSING");
            return;
        }
        if (dto->mLEvent && dto->mLEvent->value != sbi_gen::NwdafEvent::NF_LOAD) {
            err = problem(400,
                          "Bad Request",
                          "this AnLF monitors the accuracy of NF_LOAD models only",
                          "INVALID_MSG_FORMAT");
            return;
        }
        record = json::parse(req.body);
        record.erase("immReport");
    };
    if (is_anlf) {
        server.add_route(
            "POST",
            std::string(kMonitorRoot) + "/subscriptions",
            [&](const sbi_core::http2::Request& req) {
                json record;
                std::optional<sbi_core::http2::Response> err;
                monitor_validate(req, record, err);
                if (err) {
                    return *err;
                }
                const auto id = accuracy.create_subscription(record);
                json body = record;
                if (record.value("eventReportReq", json::object()).value("immRep", false)) {
                    if (const auto rep = monitor_report(record)) {
                        body["immReport"] = *rep;
                    }
                }
                sbi_core::http2::Response resp;
                resp.status = 201;
                resp.headers.emplace("content-type", "application/json");
                resp.headers.emplace("location",
                                     std::string(kMonitorRoot) + "/subscriptions/" + id);
                resp.body = body.dump();
                spdlog::info("nwdaf: the MTLF monitors the accuracy of model(s) {} "
                             "({})",
                             record.at("modelIds").dump(),
                             id);
                return resp;
            });
        server.add_route(
            "PUT",
            std::string(kMonitorRoot) + "/subscriptions/{subscriptionId}",
            [&](const sbi_core::http2::Request& req) {
                const auto id = req.path_params.at("subscriptionId");
                const auto existing = accuracy.get_subscription(id);
                if (!existing) {
                    return sbi_core::http2::problem_response(
                        404, "Not Found", "no ML model monitoring subscription " + id);
                }
                json record;
                std::optional<sbi_core::http2::Response> err;
                monitor_validate(req, record, err);
                if (err) {
                    return *err;
                }
                if (!accuracy.replace_subscription(id, record)) {
                    return sbi_core::http2::problem_response(
                        404, "Not Found", "no ML model monitoring subscription " + id);
                }
                json body = record;
                if (record.value("eventReportReq", json::object()).value("immRep", false)) {
                    if (const auto rep = monitor_report(record)) {
                        body["immReport"] = *rep;
                    }
                }
                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", "application/json");
                resp.body = body.dump();
                return resp;
            });
        server.add_route("DELETE",
                         std::string(kMonitorRoot) + "/subscriptions/{subscriptionId}",
                         [&](const sbi_core::http2::Request& req) {
                             if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                                 return sbi_core::http2::problem_response(
                                     401, "Unauthorized", auth->error);
                             }
                             const auto id = req.path_params.at("subscriptionId");
                             if (!accuracy.remove_subscription(id)) {
                                 return sbi_core::http2::problem_response(
                                     404, "Not Found", "no ML model monitoring subscription " + id);
                             }
                             sbi_core::http2::Response resp;
                             resp.status = 204;
                             return resp;
                         });
    }

    // ---- Nnwdaf_DataManagement_Fetch (the fetchUri this NWDAF hands out) --------------------
    server.add_route(
        "POST", std::string(kInboundPrefix) + "/fetch", [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto ids = sbi_core::http2::parse_json_body<std::vector<std::string>>(req, err);
            if (!ids) {
                return err;
            }
            if (ids->empty()) {
                return problem(400,
                               "Bad Request",
                               "fetch correlation identifiers shall not be empty",
                               "MANDATORY_IE_MISSING");
            }
            // Merge every buffered notification into one (4.4.2.5.2: "find the data").
            json merged;
            json nrf_notifs = json::array();
            for (const auto& id : *ids) {
                if (auto n = collected.take_fetch(id)) {
                    if (merged.is_null()) {
                        merged = *n;
                    }
                    for (const auto& ev : n->value("dataNotification", json::object())
                                              .value("nrfEventNotifs", json::array())) {
                        nrf_notifs.push_back(ev);
                    }
                }
            }
            if (merged.is_null()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no buffered notification for those fetch correlation ids");
            }
            merged["dataNotification"]["nrfEventNotifs"] = nrf_notifs;
            merged["notifTimestamp"] = sbi_core::format_rfc3339(std::chrono::system_clock::now());
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = merged.dump();
            return resp;
        });

    // Which source serves an NnwdafDataManagementSubsc, or why it cannot be served.
    const auto serving_source = [&](const json& request,
                                    std::optional<sbi_core::http2::Response>& err) -> std::string {
        if (request.contains("anaSub") == request.contains("dataSub")) {
            err =
                problem(400,
                        "Bad Request",
                        "exactly one of anaSub, dataSub (TS 29.520 NnwdafDataManagementSubsc "
                        "oneOf)",
                        request.contains("anaSub") ? "INVALID_MSG_FORMAT" : "MANDATORY_IE_MISSING");
            return "";
        }
        if (request.contains("dataSub")) {
            const json& ds = request.at("dataSub");
            if (ds.size() == 1 && ds.contains("nrfDataSub")) {
                return kNrfSource;
            }
            dm_rejected_counter->Add(1);
            err = problem(400,
                          "Bad Request",
                          "this NWDAF collects NRF data (nrfDataSub) only; it has no existing or "
                          "constructible subscription to serve that data source (TS 29.520 "
                          "4.4.2.2.2 NOTE 1)",
                          "SUBSCRIPTION_CANNOT_BE_SERVED");
            return "";
        }
        const json& ana = request.at("anaSub");
        for (const auto& es : ana.value("eventSubscriptions", json::array())) {
            if (es.value("event", "") != "NF_LOAD") {
                dm_rejected_counter->Add(1);
                err = problem(400,
                              "Bad Request",
                              "the input data this NWDAF collects serves NF_LOAD; no collection "
                              "exists for the input of " +
                                  es.value("event", "?") + " (TS 29.520 4.4.2.2.2 NOTE 1)",
                              "SUBSCRIPTION_CANNOT_BE_SERVED");
                return "";
            }
            if (consent_policy == "consumer-checked" && es.contains("tgtUe") &&
                (es.at("tgtUe").contains("supis") || es.at("tgtUe").contains("gpsis")) &&
                !request.value("checkedConsentInd", false)) {
                dm_rejected_counter->Add(1);
                err = problem(403,
                              "Forbidden",
                              "per-UE data collection requires checkedConsentInd under this "
                              "NWDAF's user_consent_policy; the Nudm_SDM consent check is not "
                              "built (ADR-0368)",
                              "USER_CONSENT_NOT_GRANTED");
                return "";
            }
        }
        return kNrfSource;
    };

    // The historical slice a subscription's timePeriod asks for, delivered right after the
    // response (TS 23.288 6.2.6.2 step 5: "available or collected data").
    const auto deliver_history = [&](const std::string& id,
                                     const nwdaf::DataManagementSubscription& sub) {
        if (!sub.request.contains("timePeriod")) {
            return;
        }
        const auto& tp = sub.request.at("timePeriod");
        const auto start = sbi_core::parse_rfc3339(tp.value("startTime", ""));
        const auto stop = sbi_core::parse_rfc3339(tp.value("stopTime", ""));
        if (!start || !stop) {
            return;
        }
        std::thread([&, id, sub, start, stop]() {
            try {
                dm_notify(id, sub, collected.events(sub.source, *start, *stop));
            } catch (const std::exception& e) {
                spdlog::warn("nwdaf: historical delivery for {} failed: {}", id, e.what());
            }
        }).detach();
    };

    // ---- Nnwdaf_DataManagement_Subscribe: create (4.4.2.2.2) ------------------------------------
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/subscriptions",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto =
                sbi_core::http2::parse_json_body<sbi_gen::NnwdafDataManagementSubsc>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            std::optional<sbi_core::http2::Response> bad;
            const auto source = serving_source(request, bad);
            if (bad) {
                return *bad;
            }
            for (const char* ignored : {"adrfId",
                                        "adrfSetId",
                                        "targetNfId",
                                        "targetNfSetId",
                                        "storeHandl",
                                        "notifEndpoints",
                                        "procInstruct",
                                        "multiProcInstructs"}) {
                if (request.contains(ignored)) {
                    spdlog::warn("nwdaf: {} present in the request -- accepted, not applied "
                                 "(ADR-0368)",
                                 ignored);
                }
            }
            nwdaf::DataManagementSubscription sub;
            sub.source = source;
            sub.request = request;
            const auto id = collected.create_dm_subscription(sub);
            deliver_history(id, sub);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kDataManagementRoot) + "/subscriptions/" + id);
            resp.body = request.dump();
            return resp;
        });

    // ---- Nnwdaf_DataManagement_Subscribe: update (4.4.2.2.3) ------------------------------------
    server.add_route(
        "PUT",
        std::string(kDataManagementRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto =
                sbi_core::http2::parse_json_body<sbi_gen::NnwdafDataManagementSubsc>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            std::optional<sbi_core::http2::Response> bad;
            const auto source = serving_source(request, bad);
            if (bad) {
                return *bad;
            }
            const auto id = req.path_params.at("subscriptionId");
            nwdaf::DataManagementSubscription sub;
            sub.source = source;
            sub.request = request;
            if (!collected.replace_dm_subscription(id, sub)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no such data management subscription");
            }
            deliver_history(id, sub);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = request.dump();
            return resp;
        });

    // ---- Nnwdaf_DataManagement_Unsubscribe (4.4.2.3.2) ------------------------------------------
    server.add_route(
        "DELETE",
        std::string(kDataManagementRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!collected.remove_dm_subscription(req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no such data management subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- the collector: this NWDAF's subscription at the DCCF (TS 23.288 6.2.6.3.4) ------------
    // One replica holds the collection (SET NX on nwdaf:coll:nrf) and keeps its heartbeat key
    // alive; the others watch that key and take the collection over when it lapses. A graceful
    // shutdown of the holder unsubscribes at the DCCF (which, as the last consumer, unsubscribes
    // the NRF and deconfigures the MFAF) and releases the record.
    std::atomic<bool> running{true};
    std::atomic<bool> holding{false};
    const auto pause = [&](std::int64_t seconds) {
        for (std::int64_t i = 0; i < seconds * 5 && running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return running.load();
    };
    const auto dccf_call =
        [&](const std::string& method, const std::string& url, const std::optional<json>& body) {
            struct Out {
                long status = -1;
                std::string location;
                std::string error;
            } out;
            sbi_core::http2::ClientRequest req;
            req.method = method;
            req.url = url;
            if (body) {
                req.headers.emplace("content-type", "application/json");
                req.body = body->dump();
            }
            const std::lock_guard<std::mutex> lock(client_mutex);
            auto token = oauth_dccf.get_bearer_token();
            if (!token) {
                out.error = token.error();
                return out;
            }
            req.headers.emplace("authorization", "Bearer " + *token);
            if (auto resp = client.send(req); resp) {
                out.status = resp->status;
                if (const auto it = resp->headers.find("location"); it != resp->headers.end()) {
                    out.location = it->second;
                }
            } else {
                out.error = resp.error();
            }
            return out;
        };
    // Subscribes at the DCCF and records the collection; true when this replica now holds it.
    const auto take_collection = [&](const json& body) {
        const auto r = dccf_call("POST",
                                 dccf_base + kDccfDataManagementRoot + "/data-subscriptions",
                                 std::optional<json>(body));
        if (r.status != 201) {
            return std::make_pair(false, r.status > 0 ? std::to_string(r.status) : r.error);
        }
        std::string location = r.location;
        if (!location.starts_with("http")) {
            location = dccf_base + location;
        }
        if (collected.open_collection(kNrfSource,
                                      json{{"resourceUri", location},
                                           {"replica", instance_id},
                                           {"selfBase", self_base}})) {
            collected.touch_holder(kNrfSource,
                                   std::chrono::milliseconds(collector_heartbeat_seconds * 3000));
            holding = true;
            spdlog::info("nwdaf: collecting NRF NF-status data via the DCCF ({})", location);
            return std::make_pair(true, std::string());
        }
        // Lost the race to another replica: release ours, keep theirs.
        static_cast<void>(dccf_call("DELETE", location, std::nullopt));
        return std::make_pair(false, std::string("another replica opened it first"));
    };
    std::thread collector;
    if (!is_anlf) {
        spdlog::info("nwdaf: role mtlf -- no data collection here; the MTLF trains on what the "
                     "ADRF holds (ADR-0369)");
    } else if (collect_via_dccf) {
        collector = std::thread([&, collect_nrf_nf_types]() {
            json nrf_sub{{"nfStatusNotificationUri",
                          self_base + kInboundPrefix + "/notifications/" + kNrfSource}};
            if (!collect_nrf_nf_types.empty()) {
                // TS 29.510 SubscriptionData.subscrCond: one NF type per subscription; the first
                // configured type is subscribed here, the rest are disclosed as not applied
                // (this project's NRF fans every status change out regardless, ADR-0079).
                nrf_sub["subscrCond"] = json{{"nfType", collect_nrf_nf_types.front()}};
                if (collect_nrf_nf_types.size() > 1) {
                    spdlog::warn("nwdaf: data_collection.nrf_status_nf_types names {} types; one "
                                 "NRF subscription carries one subscrCond -- subscribing {} only",
                                 collect_nrf_nf_types.size(),
                                 collect_nrf_nf_types.front());
                }
            }
            const json body{
                {"dataSub", json{{"nrfDataSub", nrf_sub}}},
                {"dataNotifUri", self_base + kInboundPrefix + "/notifications/" + kNrfSource},
                {"dataNotifCorrId", kNrfSource}};
            int attempt = 0;
            do {
                try {
                    if (holding) {
                        collected.touch_holder(
                            kNrfSource,
                            std::chrono::milliseconds(collector_heartbeat_seconds * 3000));
                        continue;
                    }
                    const auto held = collected.get_collection(kNrfSource);
                    if (held && collected.holder_alive(kNrfSource)) {
                        if (attempt++ == 0) {
                            spdlog::info("nwdaf: NRF collection held by replica {} -- reading its "
                                         "timeline",
                                         held->value("replica", "?"));
                        }
                        continue;
                    }
                    if (held) {
                        // The holder died: its subscription at the DCCF delivers to nobody.
                        spdlog::info("nwdaf: NRF collection holder {} is gone -- taking over",
                                     held->value("replica", "?"));
                        static_cast<void>(
                            dccf_call("DELETE", held->value("resourceUri", ""), std::nullopt));
                        collected.close_collection(kNrfSource);
                    }
                    const auto [ok, why] = take_collection(body);
                    if (!ok && attempt++ % 12 == 0) {
                        spdlog::warn("nwdaf: DCCF subscription for NRF data not accepted yet ({}), "
                                     "retrying",
                                     why);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("nwdaf: collector: {}", e.what());
                }
            } while (pause(collector_heartbeat_seconds));
            if (holding) {
                try {
                    if (const auto held = collected.get_collection(kNrfSource)) {
                        static_cast<void>(
                            dccf_call("DELETE", held->value("resourceUri", ""), std::nullopt));
                    }
                    collected.close_collection(kNrfSource);
                    spdlog::info("nwdaf: NRF collection released at the DCCF on shutdown");
                } catch (const std::exception& e) {
                    spdlog::warn("nwdaf: could not release the NRF collection: {}", e.what());
                }
            }
        });
    } else {
        spdlog::info("nwdaf: data_collection.via_dccf is false -- NF_LOAD from NRF discovery "
                     "snapshots only");
    }

    // ---- notifier: every subscription with a notificationURI gets its events on the interval --
    // Lease slightly shorter than the interval so the next tick can always claim afresh; the
    // tick itself is not synchronised across replicas, which is exactly why the lease is needed.
    const auto notify_lease = std::chrono::milliseconds(notify_interval_seconds * 1000 * 9 / 10);
    // Joined on shutdown, not detached (ADR-0368): it uses the stores and the client that main
    // destroys on the way out.
    std::thread notifier([&] {
        while (is_anlf && pause(notify_interval_seconds)) {
            std::vector<std::pair<std::string, sbi_gen::NnwdafEventsSubscription>> snapshot;
            try {
                for (auto& [id, sub] : store.all_subscriptions()) {
                    if (sub.notificationURI) {
                        snapshot.emplace_back(id, std::move(sub));
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("nwdaf: notifier could not read subscriptions from Valkey: {}",
                              e.what());
                continue;
            }
            for (const auto& [id, sub] : snapshot) {
                // ADR-0365: exactly one replica delivers this subscription this interval.
                try {
                    if (!store.claim_notification(id, notify_lease)) {
                        continue;
                    }
                } catch (const std::exception& e) {
                    spdlog::error("nwdaf: notifier could not claim lease for {}: {}", id, e.what());
                    continue;
                }
                sbi_gen::NnwdafEventsSubscriptionNotification note;
                note.subscriptionId = id;
                note.notifCorrId = sub.notifCorrId;
                note.eventNotifications =
                    std::vector<sbi_gen::EventNotification_Nnwdaf_EventsSubscription>();
                for (const auto& es : sub.eventSubscriptions) {
                    std::optional<sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo> filter;
                    if (es.nfTypes || es.nfInstanceIds) {
                        filter = sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo{};
                        filter->nfTypes = es.nfTypes;
                        filter->nfInstanceIds = es.nfInstanceIds;
                    }
                    std::optional<sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo> data;
                    {
                        const std::lock_guard<std::mutex> lock(client_mutex);
                        data = compute(es.event.value,
                                       filter,
                                       es.tgtUe,
                                       es.extraReportReq,
                                       analytics,
                                       client,
                                       oauth,
                                       nrf_base,
                                       features,
                                       features_mutex,
                                       collected,
                                       ml);
                    }
                    sbi_gen::EventNotification_Nnwdaf_EventsSubscription en;
                    en.event = es.event;
                    en.timeStampGen = sbi_core::format_rfc3339(std::chrono::system_clock::now());
                    if (data) {
                        en.nfLoadLevelInfos = data->nfLoadLevelInfos;
                        en.abnorBehavrs = data->abnorBehavrs;
                        en.svcExps = data->svcExps;
                    } else {
                        // The YAML's own way to say "could not produce this one": NwdafFailureCode.
                        sbi_gen::NwdafFailureCode fc;
                        fc.value = sbi_gen::NwdafFailureCode::UNAVAILABLE_DATA;
                        en.failNotifyCode = fc;
                    }
                    note.eventNotifications->push_back(std::move(en));
                }
                sbi_core::http2::ClientRequest req;
                req.method = "POST";
                req.url = *sub.notificationURI;
                req.headers.emplace("content-type", "application/json");
                req.headers.emplace(sbi_core::headers::kCallback, kEventsCallback);
                req.body = json(note).dump();
                std::optional<int> status;
                {
                    const std::lock_guard<std::mutex> lock(client_mutex);
                    if (auto resp = client.send(req); resp) {
                        status = resp->status;
                    }
                }
                if (status && *status >= 200 && *status < 300) {
                    notify_counter->Add(1);
                } else {
                    spdlog::warn("nwdaf: notification for {} to {} failed ({})",
                                 id,
                                 *sub.notificationURI,
                                 status.value_or(-1));
                }
            }
        }
    });

    // ---- ADR-0370: judge matured predictions against the observed loads, and notify the
    // monitor subscriptions (6.2E.3.3 steps 4-6): when the accuracy no longer meets the
    // subscription's threshold (and once more when it does again), or on a PERIODIC repPeriod.
    // One replica reports a subscription per tick (lease); the numbers are shared.
    std::thread accuracy_thread([&] {
        while (is_anlf && ml != nullptr && pause(acc_check_interval_seconds)) {
            try {
                const auto now = std::chrono::system_clock::now();
                std::vector<nwdaf::LoadObservation> loads;
                for (const auto& e : collected.events(
                         kNrfSource, now - std::chrono::seconds(acc_window_seconds), now)) {
                    if (auto o = observation_from_nrf(e); o && o->load) {
                        loads.push_back(nwdaf::LoadObservation{o->nf_instance_id, o->at, *o->load});
                    }
                }
                accuracy.evaluate(sbi_gen::NwdafEvent::NF_LOAD, loads);
                const auto lease =
                    std::chrono::milliseconds(acc_check_interval_seconds * 1000 * 9 / 10);
                for (auto& [id, sub] : accuracy.all_subscriptions()) {
                    if (!accuracy.claim_report(id, lease)) {
                        continue;
                    }
                    const auto rep = monitor_report(sub);
                    if (!rep) {
                        continue;
                    }
                    const bool meets = rep->value("accuMeetInd", true);
                    const auto last_at = sbi_core::parse_rfc3339(sub.value("lastReportAt", ""));
                    const auto since_last =
                        last_at ? now - *last_at : std::chrono::system_clock::duration::max();
                    const auto& erq = sub.value("eventReportReq", json::object());
                    bool due = false;
                    if (erq.value("notifMethod", "") ==
                            sbi_gen::NotificationMethod_Nsmf_EventExposure::PERIODIC &&
                        erq.contains("repPeriod")) {
                        due = since_last >=
                              std::chrono::seconds(erq.at("repPeriod").get<std::int64_t>());
                    } else {
                        // Threshold semantics: report while insufficient (rate-limited), and
                        // once when it meets the threshold again.
                        const auto last_meet = sub.contains("lastMeet")
                                                   ? std::optional<bool>(sub.at("lastMeet"))
                                                   : std::nullopt;
                        due = (!meets && since_last >= std::chrono::seconds(
                                                           acc_min_report_interval_seconds)) ||
                              (meets && last_meet.has_value() && !*last_meet);
                    }
                    if (!due) {
                        continue;
                    }
                    sbi_core::http2::ClientRequest req;
                    req.method = "POST";
                    req.url = sub.at("notificationUri").get<std::string>();
                    req.headers.emplace("content-type", "application/json");
                    req.headers.emplace(sbi_core::headers::kCallback, kMonitorCallback);
                    req.body = rep->dump();
                    std::optional<int> status;
                    {
                        const std::lock_guard<std::mutex> lock(client_mutex);
                        if (auto resp = client.send(req); resp) {
                            status = static_cast<int>(resp->status);
                        }
                    }
                    if (status && *status >= 200 && *status < 300) {
                        sub["lastReportAt"] = sbi_core::format_rfc3339(now);
                        sub["lastMeet"] = meets;
                        accuracy.replace_subscription(id, sub);
                        spdlog::info("nwdaf: accuracy report for {} sent ({})",
                                     id,
                                     meets ? "meets the threshold" : "below threshold");
                    } else {
                        spdlog::warn("nwdaf: Nnwdaf_MLModelMonitor_Notify for {} to {} failed ({})",
                                     id,
                                     req.url,
                                     status.value_or(-1));
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("nwdaf: accuracy monitoring tick failed: {}", e.what());
            }
        }
    });

    json nwdaf_info = json::object();
    std::vector<std::string> service_names;
    if (is_anlf) {
        service_names.insert(service_names.end(),
                             {"nnwdaf-analyticsinfo",
                              "nnwdaf-eventssubscription",
                              "nnwdaf-datamanagement",
                              "nnwdaf-mlmodelmonitor"});
    }
    if (is_mtlf) {
        service_names.push_back("nnwdaf-mlmodelprovision");
        if (!is_anlf) {
            service_names.push_back("nnwdaf-mlmodelmonitor");
        }
    }
    if (is_anlf) {
        nwdaf_info["eventIds"] = json::array({sbi_gen::NwdafEvent::NF_LOAD,
                                              sbi_gen::NwdafEvent::ABNORMAL_BEHAVIOUR,
                                              sbi_gen::NwdafEvent::SERVICE_EXPERIENCE});
    }
    if (is_mtlf) {
        nwdaf_info.update(mtlf.nrf_profile_info());
    }
    std::thread(run_nrf_lifecycle,
                instance_id,
                nrf_base,
                advertised_ipv4,
                port,
                heartbeat_seconds,
                nwdaf_info,
                service_names)
        .detach();
    // ADR-0369: the AnLF's ML-model subscription holder, and the MTLF's training loop.
    std::thread ml_thread([&] {
        if (ml) {
            ml_consumer.run(running, pause);
        }
    });
    std::thread mtlf_thread([&] {
        if (is_mtlf) {
            mtlf.run(running, pause);
        }
    });

    // ADR-0379 slice 2: subscribe to the SMF's Nsmf_EventExposure QOS_MON, delivered to this
    // NWDAF's /qos-mon inbound route. One replica creates the subscription (open_collection lock);
    // it outlives the replica by design -- no takeover/unsubscribe in this slice, disclosed, the
    // same shape as the DCCF collection. Retries until the SMF answers 201.
    std::thread qos_mon_sub_thread([&] {
        if (!smf_base ||
            !collected.open_collection(kSmfQosMonSource,
                                       json{{"replica", instance_id}, {"selfBase", self_base}})) {
            return; // not configured, or another replica holds the QOS_MON subscription
        }
        const json subscription{{"notifId", "nwdaf-qosmon-" + instance_id},
                                {"notifUri", self_base + std::string(kInboundPrefix) + "/qos-mon"},
                                {"eventSubs", json::array({json{{"event", "QOS_MON"}}})}};
        while (running.load()) {
            long status = -1;
            std::string error;
            {
                const std::lock_guard<std::mutex> lock(client_mutex);
                auto token = oauth_smf.get_bearer_token();
                sbi_core::http2::ClientRequest req;
                req.method = "POST";
                req.url = *smf_base + "/nsmf-event-exposure/v1/subscriptions";
                req.headers.emplace("content-type", "application/json");
                if (token) {
                    req.headers.emplace("authorization", "Bearer " + *token);
                }
                req.body = subscription.dump();
                if (auto resp = client.send(req); resp) {
                    status = resp->status;
                } else {
                    error = resp.error();
                }
            }
            if (status == 201) {
                spdlog::info("nwdaf: subscribed to the SMF's Nsmf_EventExposure QOS_MON at {}",
                             *smf_base);
                return;
            }
            spdlog::warn(
                "nwdaf: SMF QOS_MON subscription not accepted yet (status {} {}), retrying",
                status,
                error);
            if (!pause(10)) {
                return;
            }
        }
    });

    server.start();
    spdlog::info("nwdaf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("nwdaf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    running = false;
    notifier.join();
    ml_thread.join(); // unsubscribes at the MTLF when this replica holds the subscription
    accuracy_thread.join();
    mtlf_thread.join(); // a training in flight finishes or times out before the stores go
    if (collector.joinable()) {
        collector.join(); // releases the collection at the DCCF before the stores go away
    }
    if (qos_mon_sub_thread.joinable()) {
        qos_mon_sub_thread.join();
    }
    return 0;
}
