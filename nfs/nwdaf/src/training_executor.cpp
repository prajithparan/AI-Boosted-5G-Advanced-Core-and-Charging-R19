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
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

} // namespace

SubprocessExecutor::SubprocessExecutor(SubprocessExecutorOptions options)
    : options_(std::move(options)) {}

std::expected<TrainingResult, std::string> SubprocessExecutor::train(const TrainingJob& job) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(options_.workdir, ec);
    if (ec) {
        return std::unexpected("training workdir " + options_.workdir + ": " + ec.message());
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
            return std::unexpected("cannot write " + dataset.string());
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
    posix_spawn_file_actions_addchdir_np(&actions, options_.workdir.c_str());
    pid_t pid = 0;
    const int rc =
        posix_spawnp(&pid, options_.python.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        return std::unexpected("cannot start " + options_.python + ": " + std::strerror(rc));
    }
    spdlog::info("nwdaf: training {} started (pid {}, {} -> {})",
                 job.event,
                 pid,
                 dataset.filename().string(),
                 model.filename().string());

    const auto deadline = std::chrono::steady_clock::now() + options_.timeout;
    int status = 0;
    while (true) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        if (w < 0) {
            return std::unexpected(std::string("waitpid: ") + std::strerror(errno));
        }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return std::unexpected("training exceeded " + std::to_string(options_.timeout.count()) +
                                   "s and was killed; log tail: " + tail_of(log, 1000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(
            "training exited with " +
            (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : std::string("a signal")) +
            "; log tail: " + tail_of(log, 1500));
    }
    TrainingResult result;
    result.onnx_bytes = read_file(model);
    if (result.onnx_bytes.empty()) {
        return std::unexpected("training produced no model at " + model.string());
    }
    try {
        result.report = nlohmann::json::parse(read_file(report));
    } catch (const std::exception& e) {
        return std::unexpected("training report unreadable: " + std::string(e.what()));
    }
    fs::remove(dataset, ec); // the model and the report stay for the operator; the dataset is bulk
    return result;
}

} // namespace nwdaf
