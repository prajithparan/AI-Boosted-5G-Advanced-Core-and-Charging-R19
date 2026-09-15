#include "accuracy_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

namespace nwdaf {

using json = nlohmann::json;

namespace {
constexpr const char* kCounter = "nwdaf:next_id";
constexpr const char* kPredPrefix = "nwdaf:anlf:pred:";
constexpr const char* kOutcomePrefix = "nwdaf:anlf:outcome:";
constexpr const char* kSubPrefix = "nwdaf:anlf:monsub:";
constexpr const char* kSubIndex = "nwdaf:anlf:monsubs";
constexpr const char* kLeasePrefix = "nwdaf:anlf:monlease:";

std::int64_t epoch_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}
} // namespace

void AccuracyMonitor::record_prediction(const std::string& event,
                                        const std::string& nf_instance_id,
                                        std::int64_t model_id,
                                        double predicted,
                                        std::chrono::system_clock::time_point made_at) {
    const auto seq = redis_->incr(kCounter);
    redis_->zadd(kPredPrefix + event,
                 json{{"seq", seq},
                      {"nf", nf_instance_id},
                      {"model", model_id},
                      {"pred", predicted},
                      {"t", epoch_ms(made_at)}}
                     .dump(),
                 static_cast<double>(epoch_ms(made_at)));
}

int AccuracyMonitor::evaluate(const std::string& event,
                              const std::vector<LoadObservation>& observations) {
    // Observations per instance, time-ordered, so "the next observation after t" is a search.
    std::map<std::string, std::vector<const LoadObservation*>> by_instance;
    for (const auto& o : observations) {
        by_instance[o.nf_instance_id].push_back(&o);
    }
    for (auto& [id, v] : by_instance) {
        std::sort(v.begin(), v.end(), [](const auto* a, const auto* b) { return a->at < b->at; });
    }
    const auto now = std::chrono::system_clock::now();
    std::vector<std::string> pending;
    redis_->zrange(kPredPrefix + event, 0, -1, std::back_inserter(pending));
    int judged = 0;
    for (const auto& member : pending) {
        json p;
        try {
            p = json::parse(member);
        } catch (const json::exception&) {
            redis_->zrem(kPredPrefix + event, member);
            continue;
        }
        const auto made_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(p.value("t", std::int64_t(0))));
        const LoadObservation* truth = nullptr;
        if (const auto it = by_instance.find(p.value("nf", "")); it != by_instance.end()) {
            for (const auto* o : it->second) {
                if (o->at > made_at) {
                    truth = o;
                    break;
                }
            }
        }
        if (truth == nullptr) {
            if (now - made_at > truth_timeout_) {
                redis_->zrem(kPredPrefix + event, member); // no ground truth ever came
            }
            continue;
        }
        const double deviation = std::fabs(p.value("pred", 0.0) - static_cast<double>(truth->load));
        redis_->zadd(kOutcomePrefix + event,
                     json{{"seq", p.value("seq", std::int64_t(0))},
                          {"model", p.value("model", std::int64_t(0))},
                          {"dev", deviation},
                          {"ok", deviation <= tolerance_},
                          {"t", epoch_ms(truth->at)}}
                         .dump(),
                     static_cast<double>(epoch_ms(truth->at)));
        redis_->zrem(kPredPrefix + event, member);
        ++judged;
    }
    // The window: outcomes older than it leave.
    redis_->zremrangebyscore(
        kOutcomePrefix + event,
        sw::redis::RightBoundedInterval<double>(static_cast<double>(epoch_ms(now - window_)),
                                                sw::redis::BoundType::OPEN));
    return judged;
}

AccuracySummary AccuracyMonitor::summary(const std::string& event, std::int64_t model_id) {
    AccuracySummary s;
    s.model_id = model_id;
    std::vector<std::string> members;
    redis_->zrange(kOutcomePrefix + event, 0, -1, std::back_inserter(members));
    double dev_sum = 0;
    for (const auto& m : members) {
        try {
            const auto o = json::parse(m);
            if (model_id != 0 && o.value("model", std::int64_t(0)) != model_id) {
                continue;
            }
            ++s.inferences;
            s.correct += o.value("ok", false) ? 1 : 0;
            dev_sum += o.value("dev", 0.0);
        } catch (const json::exception&) {
        }
    }
    s.mean_abs_deviation = s.inferences == 0 ? 0 : dev_sum / static_cast<double>(s.inferences);
    return s;
}

std::string AccuracyMonitor::create_subscription(const json& record) {
    const auto id = "nwdaf-mon-" + std::to_string(redis_->incr(kCounter));
    redis_->set(kSubPrefix + id, record.dump());
    redis_->sadd(kSubIndex, id);
    return id;
}

std::optional<json> AccuracyMonitor::get_subscription(const std::string& id) {
    const auto raw = redis_->get(kSubPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return json::parse(*raw);
}

bool AccuracyMonitor::replace_subscription(const std::string& id, const json& record) {
    return redis_->set(
        kSubPrefix + id, record.dump(), std::chrono::milliseconds(0), sw::redis::UpdateType::EXIST);
}

bool AccuracyMonitor::remove_subscription(const std::string& id) {
    redis_->srem(kSubIndex, id);
    return redis_->del(kSubPrefix + id) > 0;
}

std::vector<std::pair<std::string, json>> AccuracyMonitor::all_subscriptions() {
    std::vector<std::string> ids;
    redis_->smembers(kSubIndex, std::back_inserter(ids));
    std::vector<std::pair<std::string, json>> out;
    for (const auto& id : ids) {
        if (auto s = get_subscription(id)) {
            out.emplace_back(id, std::move(*s));
        }
    }
    return out;
}

bool AccuracyMonitor::claim_report(const std::string& id, std::chrono::milliseconds ttl) {
    return redis_->set(kLeasePrefix + id, "1", ttl, sw::redis::UpdateType::NOT_EXIST);
}

} // namespace nwdaf
