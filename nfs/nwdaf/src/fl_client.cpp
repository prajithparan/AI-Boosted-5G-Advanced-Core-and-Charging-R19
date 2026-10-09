#include "fl_client.hpp"

#include <algorithm>
#include <cmath>

namespace nwdaf {

using json = nlohmann::json;

namespace {
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

std::string fl_base64_encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8);
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool fl_base64_decode(const std::string& in, std::string& out) {
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    std::size_t pad = 0;
    for (const char c : in) {
        if (c == '=') {
            ++pad;
            continue;
        }
        const auto* p = std::find(std::begin(kB64), std::end(kB64) - 1, c);
        if (pad != 0 || p == std::end(kB64) - 1) {
            return false;
        }
        acc = (acc << 6) | static_cast<unsigned>(p - std::begin(kB64));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return pad <= 2;
}

FlRoundOutcome run_fl_round(const json& sub,
                            const json& dataset,
                            FlRoundExecutor& executor,
                            const FlClientOptions& options) {
    FlRoundOutcome out;
    // A round that cannot even start (unusable global model) is reported to the FL Server the
    // same way as a failed round: DelayEventNotif with ML_MODEL_TRAIN_FAILURE (TS 29.520).
    const auto failed = [&](const std::string& why) {
        out.kind = FlRoundKind::Failure;
        out.detail = why;
        json n{{"notifCorreId", sub.value("notifCorreId", "")},
               {"mlCorreId", sub.at("mlCorreId")},
               {"delayEventNotif",
                json{{"delayEventInd", true}, {"delayCause", "ML_MODEL_TRAIN_FAILURE"}}}};
        if (sub.contains("roundInd")) {
            n["roundInd"] = sub.at("roundInd");
        }
        out.notification = n;
        return out;
    };
    if (!sub.contains("mlCorreId")) {
        return out; // NotFl
    }
    // TS 29.520 skipFlInd: "skip the current FL round". mLPreFlag: the subscription only prepares
    // training (no round yet). Neither produces a notification here -- the YAML defines none.
    if (sub.value("skipFlInd", false)) {
        out.kind = FlRoundKind::Skipped;
        out.detail = "skipFlInd";
        return out;
    }
    if (sub.value("mLPreFlag", false)) {
        out.kind = FlRoundKind::Skipped;
        out.detail = "mLPreFlag (training preparation only)";
        return out;
    }

    std::string event;
    if (sub.contains("mLEventSubscs") && !sub.at("mLEventSubscs").empty()) {
        event = sub.at("mLEventSubscs").front().value("mLEvent", "");
    }

    FlRoundJob job;
    job.event = event;
    job.dataset = dataset;
    job.epochs = options.epochs;
    job.learning_rate = options.learning_rate;
    job.accuracy_tolerance = options.accuracy_tolerance;
    if (sub.contains("mLModelInfos") && !sub.at("mLModelInfos").empty()) {
        const auto& info = sub.at("mLModelInfos").front();
        if (info.contains("mlFile")) {
            std::string raw;
            if (!fl_base64_decode(info.at("mlFile").get<std::string>(), raw)) {
                return failed("global model mlFile is not base64");
            }
            job.global_model = json::parse(raw, nullptr, false);
            if (job.global_model.is_discarded()) {
                return failed("global model mlFile is not JSON");
            }
        } else if (info.contains("mLFileAddr")) {
            return failed("global model by mLFileAddr is not implemented (only inline mlFile)");
        }
    }

    std::chrono::seconds max_res = options.default_max_response;
    if (sub.contains("mLTrainRepInfo") && sub.at("mLTrainRepInfo").contains("maxResTime")) {
        max_res = std::chrono::seconds(sub.at("mLTrainRepInfo").at("maxResTime").get<long>());
    }

    const auto started = std::chrono::steady_clock::now();
    auto result = executor.train_round(job);
    const auto took = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    const bool late = max_res.count() > 0 && took > max_res;

    json notif{{"notifCorreId", sub.value("notifCorreId", "")}, {"mlCorreId", sub.at("mlCorreId")}};
    if (sub.contains("roundInd")) {
        notif["roundInd"] = sub.at("roundInd");
    }

    if (!result || late) {
        // TS 29.520 DelayEventNotif: not able to complete within maxResTime. The cause is
        // NEED_MORE_TIME when it was a time overrun, ML_MODEL_TRAIN_FAILURE when training failed.
        out.kind = FlRoundKind::Delay;
        out.detail = result ? "round took " + std::to_string(took.count()) + "s > maxResTime " +
                                  std::to_string(max_res.count()) + "s"
                            : result.error();
        json delay{{"delayEventInd", true},
                   {"delayCause", result ? "NEED_MORE_TIME" : "ML_MODEL_TRAIN_FAILURE"}};
        if (result) {
            delay["expCompTime"] = took.count();
        }
        notif["delayEventNotif"] = delay;
        out.notification = notif;
        return out;
    }

    // The FedAvg weight travels with the model (project convention, see fl_client.hpp).
    json local_model = result->model;
    local_model["n_samples"] = result->report.value("n_samples", 0);
    json info{{"event", event},
              {"notifCorreId", sub.value("notifCorreId", "")},
              {"mlFile", fl_base64_encode(local_model.dump())}};
    notif["mLModelInfos"] = json::array({info});
    // mLAccChkFlg: accuracy of the supplied GLOBAL model on this client's data (a Uinteger).
    if (sub.value("mLAccChkFlg", false) && result->report.contains("accuracy_global_pct") &&
        result->report.at("accuracy_global_pct").is_number()) {
        notif["statusReport"] =
            json{{"mlModelAcc",
                  static_cast<long>(
                      std::lround(result->report.at("accuracy_global_pct").get<double>()))}};
    }
    out.kind = FlRoundKind::Model;
    out.detail = "n_samples=" + std::to_string(result->report.value("n_samples", 0));
    out.notification = notif;
    return out;
}

} // namespace nwdaf
