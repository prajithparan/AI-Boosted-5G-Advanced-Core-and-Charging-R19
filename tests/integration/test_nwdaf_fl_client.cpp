// ADR-0471 increment 3: the NWDAF federated-learning client round logic
// (nfs/nwdaf/src/fl_client.cpp) against TS 29.520 Nnwdaf_MLModelTraining and TS 23.288 6.2C.2.2. No
// ports, no NFs: a fake executor drives every branch, and one test runs the real fl_local_round.py
// through SubprocessFlRoundExecutor (skipped when the configure-time Python cannot import numpy).
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include "fl_client.hpp"

#include <gtest/gtest.h>

using nlohmann::json;
using namespace nwdaf;

namespace {

class FakeExecutor final : public FlRoundExecutor {
public:
    std::optional<FlRoundResult> result;
    std::string error = "boom";
    std::chrono::milliseconds delay{0};
    FlRoundJob seen;
    int calls = 0;
    tl::expected<FlRoundResult, std::string> train_round(const FlRoundJob& job) override {
        ++calls;
        seen = job;
        std::this_thread::sleep_for(delay);
        if (!result) {
            return tl::make_unexpected(error);
        }
        return *result;
    }
};

json fl_sub() {
    return json{{"mLEventSubscs", json::array({json{{"mLEvent", "NF_LOAD"}}})},
                {"notifUri", "https://127.0.0.1:1/cb"},
                {"notifCorreId", "corr-1"},
                {"mlCorreId", "fl-1"},
                {"roundInd", 3}};
}

FlRoundResult ok_result(double global_acc) {
    FlRoundResult r;
    r.model = json{{"model_type", "linear-fl-v1"}, {"bias", 0.5}};
    r.report = json{{"n_samples", 40}, {"accuracy_global_pct", global_acc}};
    return r;
}

FlClientOptions opts() {
    FlClientOptions o;
    o.epochs = 7;
    o.learning_rate = 0.1;
    o.accuracy_tolerance = 10;
    o.default_max_response = std::chrono::seconds(0);
    return o;
}

} // namespace

TEST(NwdafFlClient, Base64RoundTripsAllPaddingLengths) {
    for (const std::string& s :
         std::vector<std::string>{"", "a", "ab", "abc", "abcd", std::string("\0\xff\x80", 3)}) {
        std::string back;
        ASSERT_TRUE(fl_base64_decode(fl_base64_encode(s), back));
        EXPECT_EQ(back, s);
    }
    EXPECT_EQ(fl_base64_encode("Man"), "TWFu"); // RFC 4648 test vectors
    EXPECT_EQ(fl_base64_encode("Ma"), "TWE=");
    EXPECT_EQ(fl_base64_encode("M"), "TQ==");
    std::string out;
    EXPECT_FALSE(fl_base64_decode("TW*u", out));
    EXPECT_FALSE(fl_base64_decode("TQ==TQ", out));
}

TEST(NwdafFlClient, OrdinarySubscriptionIsNotFl) {
    FakeExecutor ex;
    auto sub = fl_sub();
    sub.erase("mlCorreId");
    EXPECT_EQ(run_fl_round(sub, json::object(), ex, opts()).kind, FlRoundKind::NotFl);
    EXPECT_EQ(ex.calls, 0);
}

TEST(NwdafFlClient, SkipFlIndAndPreparationTrainNothing) {
    FakeExecutor ex;
    auto sub = fl_sub();
    sub["skipFlInd"] = true;
    EXPECT_EQ(run_fl_round(sub, json::object(), ex, opts()).kind, FlRoundKind::Skipped);
    sub["skipFlInd"] = false;
    sub["mLPreFlag"] = true;
    EXPECT_EQ(run_fl_round(sub, json::object(), ex, opts()).kind, FlRoundKind::Skipped);
    EXPECT_EQ(ex.calls, 0);
}

TEST(NwdafFlClient, RoundProducesModelNotificationWithGlobalModelAndAccuracy) {
    FakeExecutor ex;
    ex.result = ok_result(86.6);
    auto sub = fl_sub();
    const json global{{"model_type", "linear-fl-v1"}, {"bias", 0.0}};
    sub["mLModelInfos"] = json::array({json{{"mlFile", fl_base64_encode(global.dump())}}});
    sub["mLAccChkFlg"] = true;
    const auto out = run_fl_round(sub, json{{"series", json::object()}}, ex, opts());
    ASSERT_EQ(out.kind, FlRoundKind::Model) << out.detail;
    EXPECT_EQ(ex.seen.global_model, global); // the server's model reached the trainer
    EXPECT_EQ(ex.seen.epochs, 7);
    EXPECT_EQ(ex.seen.learning_rate, 0.1);
    const auto& n = out.notification;
    EXPECT_EQ(n.at("notifCorreId"), "corr-1");
    EXPECT_EQ(n.at("mlCorreId"), "fl-1");
    EXPECT_EQ(n.at("roundInd"), 3);
    EXPECT_FALSE(n.contains("delayEventNotif"));
    EXPECT_EQ(n.at("statusReport").at("mlModelAcc"), 87); // Uinteger, rounded
    std::string raw;
    ASSERT_TRUE(fl_base64_decode(n.at("mLModelInfos").at(0).at("mlFile"), raw));
    EXPECT_EQ(json::parse(raw), ex.result->model);
    EXPECT_EQ(n.at("mLModelInfos").at(0).at("event"), "NF_LOAD");
}

TEST(NwdafFlClient, NoStatusReportUnlessAccuracyCheckRequested) {
    FakeExecutor ex;
    ex.result = ok_result(90);
    const auto out = run_fl_round(fl_sub(), json::object(), ex, opts());
    ASSERT_EQ(out.kind, FlRoundKind::Model);
    EXPECT_FALSE(out.notification.contains("statusReport"));
}

TEST(NwdafFlClient, TrainingFailureIsDelayWithFailureCause) {
    FakeExecutor ex; // result unset -> error
    const auto out = run_fl_round(fl_sub(), json::object(), ex, opts());
    ASSERT_EQ(out.kind, FlRoundKind::Delay);
    const auto& d = out.notification.at("delayEventNotif");
    EXPECT_EQ(d.at("delayEventInd"), true);
    EXPECT_EQ(d.at("delayCause"), "ML_MODEL_TRAIN_FAILURE");
    EXPECT_FALSE(out.notification.contains("mLModelInfos")); // oneOf: exactly one of the three
    EXPECT_EQ(out.detail, "boom");
}

TEST(NwdafFlClient, OverrunningMaxResTimeIsDelayNeedMoreTime) {
    FakeExecutor ex;
    ex.result = ok_result(80);
    ex.delay = std::chrono::milliseconds(2100);
    auto sub = fl_sub();
    sub["mLTrainRepInfo"] = json{{"maxResTime", 1}};
    const auto out = run_fl_round(sub, json::object(), ex, opts());
    ASSERT_EQ(out.kind, FlRoundKind::Delay) << out.detail;
    const auto& d = out.notification.at("delayEventNotif");
    EXPECT_EQ(d.at("delayCause"), "NEED_MORE_TIME");
    EXPECT_GE(d.at("expCompTime").get<int>(), 2);
    EXPECT_FALSE(out.notification.contains("mLModelInfos"));
}

TEST(NwdafFlClient, UnreadableGlobalModelIsAFailureNotAZeroStart) {
    FakeExecutor ex;
    ex.result = ok_result(80);
    auto sub = fl_sub();
    sub["mLModelInfos"] = json::array({json{{"mlFile", "!!notbase64"}}});
    const auto bad = run_fl_round(sub, json::object(), ex, opts());
    EXPECT_EQ(bad.kind, FlRoundKind::Failure);
    EXPECT_EQ(bad.notification.at("delayEventNotif").at("delayCause"), "ML_MODEL_TRAIN_FAILURE");
    EXPECT_EQ(bad.notification.at("roundInd"), 3);
    sub["mLModelInfos"] = json::array({json{{"mlFile", fl_base64_encode("not json")}}});
    EXPECT_EQ(run_fl_round(sub, json::object(), ex, opts()).kind, FlRoundKind::Failure);
    sub["mLModelInfos"] = json::array({json{{"mLFileAddr", json::object()}}});
    EXPECT_EQ(run_fl_round(sub, json::object(), ex, opts()).kind, FlRoundKind::Failure);
    EXPECT_EQ(ex.calls, 0);
}

// The real sidecar through the real executor: a ramp of NF load samples must train a finite model.
TEST(NwdafFlClient, RealSidecarRoundOnRealSeries) {
    if (std::system((std::string(FL_PYTHON) + " -c 'import numpy' >/dev/null 2>&1").c_str()) != 0) {
        GTEST_SKIP() << "numpy not importable by " << FL_PYTHON;
    }
    SubprocessFlRoundExecutorOptions eo;
    eo.python = FL_PYTHON;
    eo.script = std::string(NWDAF_TRAINING_DIR) + "/fl_local_round.py";
    eo.workdir = std::string(::testing::TempDir()) + "nwdaf-fl-client-test";
    eo.timeout = std::chrono::seconds(60);
    SubprocessFlRoundExecutor real(eo);
    // The series document as Mtlf::training_dataset() writes it: per instance, ordered NRF
    // observations {status, load} (nf_load_features.py windows_from_series).
    json series = json::object();
    for (int i = 0; i < 60; ++i) {
        series["nf-1"].push_back(json{{"status", "REGISTERED"}, {"load", 20 + (i % 30)}});
    }
    FlClientOptions o = opts();
    o.epochs = 50;
    const auto out = run_fl_round(fl_sub(), json{{"series", series}}, real, o);
    ASSERT_EQ(out.kind, FlRoundKind::Model) << out.detail;
}
