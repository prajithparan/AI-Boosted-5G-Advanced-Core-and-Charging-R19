#include "training_executor.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace nwdaf {

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string tail_of(const std::filesystem::path& p, std::size_t max_bytes) {
    auto s = read_file(p);
    if (s.size() > max_bytes) {
        s = s.substr(s.size() - max_bytes);
    }
    return s;
}

// Runs `args` (interpreter first) without a shell, stdout+stderr to `log`, killing it after
// `timeout`. Shared by the model trainer and the federated-learning round executor.
tl::expected<void, std::string> run_python(const std::vector<std::string>& argv_in,
                                           const std::string& workdir,
                                           const std::filesystem::path& log,
                                           std::chrono::seconds timeout,
                                           const char* what) {
    std::vector<std::string> args = argv_in;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawn_file_actions_addchdir_np(&actions, workdir.c_str());
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        return tl::make_unexpected("cannot start " + args[0] + ": " + std::strerror(rc));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (true) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        if (w < 0) {
            return tl::make_unexpected(std::string("waitpid: ") + std::strerror(errno));
        }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return tl::make_unexpected(std::string(what) + " exceeded " +
                                       std::to_string(timeout.count()) +
                                       "s and was killed; log tail: " + tail_of(log, 1000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return tl::make_unexpected(
            std::string(what) + " exited with " +
            (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : std::string("a signal")) +
            "; log tail: " + tail_of(log, 1500));
    }
    return {};
}

} // namespace

SubprocessExecutor::SubprocessExecutor(SubprocessExecutorOptions options)
    : options_(std::move(options)) {}

tl::expected<TrainingResult, std::string> SubprocessExecutor::train(const TrainingJob& job) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(options_.workdir, ec);
    if (ec) {
        return tl::make_unexpected("training workdir " + options_.workdir + ": " + ec.message());
    }
    const auto stamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    const fs::path base = fs::path(options_.workdir) / (job.event + "-" + stamp);
    const fs::path dataset = base.string() + ".dataset.json";
    const fs::path model = base.string() + ".onnx";
    const fs::path report = base.string() + ".report.json";
    const fs::path log = base.string() + ".log";
    {
        std::ofstream out(dataset);
        out << job.dataset.dump();
        if (!out) {
            return tl::make_unexpected("cannot write " + dataset.string());
        }
    }

    // No shell: the interpreter, the script and every argument are exec'd as given.
    std::vector<std::string> args{options_.python,
                                  options_.script,
                                  "--input",
                                  dataset.string(),
                                  "--output",
                                  model.string(),
                                  "--report",
                                  report.string(),
                                  "--min-samples",
                                  std::to_string(job.min_samples),
                                  "--accuracy-tolerance",
                                  std::to_string(job.accuracy_tolerance),
                                  "--mlflow-tracking-uri",
                                  options_.mlflow_tracking_uri};
    spdlog::info("nwdaf: training {} started ({} -> {})",
                 job.event,
                 dataset.filename().string(),
                 model.filename().string());
    if (auto ran = run_python(args, options_.workdir, log, options_.timeout, "training"); !ran) {
        return tl::make_unexpected(ran.error());
    }
    TrainingResult result;
    result.onnx_bytes = read_file(model);
    if (result.onnx_bytes.empty()) {
        return tl::make_unexpected("training produced no model at " + model.string());
    }
    try {
        result.report = nlohmann::json::parse(read_file(report));
    } catch (const std::exception& e) {
        return tl::make_unexpected("training report unreadable: " + std::string(e.what()));
    }
    fs::remove(dataset, ec); // the model and the report stay for the operator; the dataset is bulk
    return result;
}

SubprocessFlRoundExecutor::SubprocessFlRoundExecutor(SubprocessFlRoundExecutorOptions options)
    : options_(std::move(options)) {}

tl::expected<FlRoundResult, std::string>
SubprocessFlRoundExecutor::train_round(const FlRoundJob& job) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(options_.workdir, ec);
    if (ec) {
        return tl::make_unexpected("FL workdir " + options_.workdir + ": " + ec.message());
    }
    const auto stamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    const fs::path base = fs::path(options_.workdir) / ("fl-" + job.event + "-" + stamp);
    const fs::path dataset = base.string() + ".dataset.json";
    const fs::path global = base.string() + ".global.json";
    const fs::path model = base.string() + ".local.json";
    const fs::path report = base.string() + ".report.json";
    const fs::path log = base.string() + ".log";
    {
        std::ofstream out(dataset);
        out << job.dataset.dump();
        if (!out) {
            return tl::make_unexpected("cannot write " + dataset.string());
        }
    }
    std::vector<std::string> args{options_.python,
                                  options_.script,
                                  "--input",
                                  dataset.string(),
                                  "--output",
                                  model.string(),
                                  "--report",
                                  report.string(),
                                  "--epochs",
                                  std::to_string(job.epochs),
                                  "--learning-rate",
                                  std::to_string(job.learning_rate),
                                  "--accuracy-tolerance",
                                  std::to_string(job.accuracy_tolerance)};
    if (!job.global_model.is_null()) {
        std::ofstream out(global);
        out << job.global_model.dump();
        if (!out) {
            return tl::make_unexpected("cannot write " + global.string());
        }
        args.push_back("--global-model");
        args.push_back(global.string());
    }
    if (auto ran = run_python(args, options_.workdir, log, options_.timeout, "FL local round");
        !ran) {
        return tl::make_unexpected(ran.error());
    }
    FlRoundResult result;
    try {
        result.model = nlohmann::json::parse(read_file(model));
        result.report = nlohmann::json::parse(read_file(report));
    } catch (const std::exception& e) {
        return tl::make_unexpected("FL local round output unreadable: " + std::string(e.what()));
    }
    fs::remove(dataset, ec);
    fs::remove(global, ec);
    return result;
}

SubprocessFlAggregator::SubprocessFlAggregator(SubprocessFlAggregatorOptions options)
    : options_(std::move(options)) {}

tl::expected<nlohmann::json, std::string>
SubprocessFlAggregator::aggregate(const std::vector<nlohmann::json>& updates) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(options_.workdir, ec);
    if (ec) {
        return tl::make_unexpected("FL workdir " + options_.workdir + ": " + ec.message());
    }
    const auto stamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    const fs::path base = fs::path(options_.workdir) / ("fl-aggregate-" + stamp);
    std::vector<std::string> args{options_.python, options_.script, "--inputs"};
    std::vector<fs::path> inputs;
    for (std::size_t i = 0; i < updates.size(); ++i) {
        inputs.push_back(base.string() + "." + std::to_string(i) + ".update.json");
        std::ofstream out(inputs.back());
        out << updates[i].dump();
        if (!out) {
            return tl::make_unexpected("cannot write " + inputs.back().string());
        }
        args.push_back(inputs.back().string());
    }
    const fs::path output = base.string() + ".global.json";
    const fs::path log = base.string() + ".log";
    args.push_back("--output");
    args.push_back(output.string());
    if (auto ran = run_python(args, options_.workdir, log, options_.timeout, "FL aggregation");
        !ran) {
        return tl::make_unexpected(ran.error());
    }
    try {
        auto model = nlohmann::json::parse(read_file(output));
        for (const auto& in : inputs) {
            fs::remove(in, ec);
        }
        return model;
    } catch (const std::exception& e) {
        return tl::make_unexpected("FL aggregation output unreadable: " + std::string(e.what()));
    }
}

} // namespace nwdaf
