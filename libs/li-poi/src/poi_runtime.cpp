#include "li_poi/poi_runtime.hpp"

#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"

#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "li_core/x1_server.hpp"
#include "li_core/x2x3_client.hpp"

namespace li_poi {

namespace x1 = li_core::x1;

namespace {
constexpr const char* kX1Path = "/X1/NE";
} // namespace

std::optional<std::array<std::uint8_t, 16>> uuid_to_bytes(const std::string& uuid) {
    std::array<std::uint8_t, 16> out{};
    std::size_t n = 0;
    int high = -1;
    for (const char c : uuid) {
        if (c == '-') {
            continue;
        }
        int v = 0;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return std::nullopt;
        }
        if (high < 0) {
            high = v;
        } else {
            if (n >= out.size()) {
                return std::nullopt;
            }
            out[n++] = static_cast<std::uint8_t>((high << 4) | v);
            high = -1;
        }
    }
    if (n != out.size() || high != -1) {
        return std::nullopt;
    }
    return out;
}

struct PoiRuntime::Impl {
    Impl(Config cfg, Hooks h)
        : config(std::move(cfg)), hooks(std::move(h)), x2_client(make_x2_config(config)),
          keepalive(make_keepalive_config(config)) {
        if (!hooks.normalise) {
            hooks.normalise = [](const std::string& s) { return s; };
        }
    }

    static li_core::X2X3ClientConfig make_x2_config(const Config& c) {
        li_core::X2X3ClientConfig x2;
        x2.host = c.mdf2_host;
        x2.port = c.mdf2_port;
        x2.client_cert_path = c.cert_path;
        x2.client_key_path = c.key_path;
        x2.ca_path = c.ca_path;
        x2.sni = c.mdf2_sni;
        return x2;
    }

    static x1::KeepaliveMonitor::Config make_keepalive_config(const Config& c) {
        x1::KeepaliveMonitor::Config k;
        k.time_p1 = std::chrono::seconds(c.x1_keepalive_p1_seconds);
        k.time_p2 = std::chrono::seconds(c.x1_keepalive_p2_seconds);
        k.time_p3 = std::chrono::seconds(c.x1_keepalive_p3_seconds);
        k.allow_deactivate_all = c.x1_allow_deactivate_all;
        return k;
    }

    bool supported(x1::TargetIdentifierKind kind) const {
        if (hooks.supported_kinds.empty()) {
            return true;
        }
        for (const auto k : hooks.supported_kinds) {
            if (k == kind) {
                return true;
            }
        }
        return false;
    }

    // --- target store (provisioned over LI_X1) -----------------------------------------------
    // xid -> the warrant's target identifiers. Process-lifetime, ADMF-provisioned; entirely
    // separate from the NF's own per-UE state. A single configured MDF2 is the delivery
    // destination, so the X1 Destination operations are accepted but not used for routing
    // (disclosed).
    std::optional<x1::ErrorCode> validate(const x1::TaskDetails& details) const {
        if (details.xid.empty() || details.targets.empty()) {
            return x1::ErrorCode::GenericError;
        }
        for (const auto& t : details.targets) {
            if (!supported(t.kind)) {
                return x1::ErrorCode::UnsupportedTargetIdentifier; // 3010
            }
        }
        return std::nullopt;
    }

    std::optional<x1::ErrorCode> activate(const x1::TaskDetails& details) {
        if (const auto bad = validate(details)) {
            return bad;
        }
        {
            const std::lock_guard<std::mutex> lock(store_mutex);
            if (tasks.count(details.xid) != 0) {
                return x1::ErrorCode::XidAlreadyExists; // table 6.7-3: 2010
            }
            tasks.emplace(details.xid,
                          TaskInfo{details.targets,
                                   details.identifier_association_events,
                                   details.delivery,
                                   details.dids});
        }
        enqueue_added(details.xid, details.targets);
        return std::nullopt;
    }

    std::optional<x1::ErrorCode> modify(const x1::TaskDetails& details) {
        if (const auto bad = validate(details)) {
            return bad;
        }
        std::vector<x1::TargetIdentifier> added;
        {
            const std::lock_guard<std::mutex> lock(store_mutex);
            // A target identifier the warrant did not carry before is "a new interception for a
            // UE"; one it already carried is not.
            const auto existing = tasks.find(details.xid);
            for (const auto& target : details.targets) {
                bool known = false;
                if (existing != tasks.end()) {
                    for (const auto& old : existing->second.targets) {
                        known =
                            known || (old.element == target.element && old.value == target.value);
                    }
                }
                if (!known) {
                    added.push_back(target);
                }
            }
            // A ModifyTask carries a full TaskDetails, so it replaces the gating too.
            tasks[details.xid] = TaskInfo{details.targets,
                                          details.identifier_association_events,
                                          details.delivery,
                                          details.dids};
        }
        enqueue_added(details.xid, added);
        return std::nullopt;
    }

    std::optional<x1::ErrorCode> deactivate(const std::string& xid) {
        {
            const std::lock_guard<std::mutex> lock(store_mutex);
            tasks.erase(xid);
            sequence.erase(xid);
        }
        enqueue_removed(xid);
        return std::nullopt;
    }

    std::optional<x1::ErrorCode> deactivate_all() {
        std::vector<std::string> xids;
        {
            const std::lock_guard<std::mutex> lock(store_mutex);
            for (const auto& [xid, task] : tasks) {
                xids.push_back(xid);
            }
            tasks.clear();
            sequence.clear();
        }
        for (const auto& xid : xids) {
            enqueue_removed(xid);
        }
        return std::nullopt;
    }

    void enqueue_removed(const std::string& xid) {
        if (!hooks.on_task_removed) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(job_mutex);
            jobs.push_back(Job{xid, {}, true});
        }
        job_cv.notify_one();
    }

    std::uint32_t next_sequence(const std::string& xid) {
        const std::lock_guard<std::mutex> lock(store_mutex);
        return sequence[xid]++;
    }

    void enqueue_added(const std::string& xid, const std::vector<x1::TargetIdentifier>& added) {
        if (added.empty() || !hooks.on_targets_added) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(job_mutex);
            jobs.push_back(Job{xid, added});
        }
        job_cv.notify_one();
    }

    void worker_loop() {
        std::unique_lock<std::mutex> lock(job_mutex);
        while (true) {
            job_cv.wait(lock, [this] { return !running.load() || !jobs.empty(); });
            if (!running.load()) {
                return;
            }
            Job job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();
            try {
                if (job.removed) {
                    hooks.on_task_removed(job.xid);
                } else {
                    hooks.on_targets_added(job.xid, job.added);
                }
            } catch (const std::exception& e) {
                spdlog::error("{}: targets-added handler failed: {}", hooks.log_name, e.what());
            }
            lock.lock();
        }
    }

    x1::TaskStoreCallbacks make_callbacks() {
        x1::TaskStoreCallbacks cb;
        cb.ne_identifier = config.ne_identifier;
        cb.keepalive_supported = true;
        cb.activate_task = [this](const x1::TaskDetails& d) { return activate(d); };
        cb.modify_task = [this](const x1::TaskDetails& d) { return modify(d); };
        cb.deactivate_task = [this](const std::string& xid) { return deactivate(xid); };
        cb.deactivate_all_tasks = [this] { return deactivate_all(); };
        // A single configured MDF2 is the delivery destination; Destination provisioning is
        // accepted (a conformant NE answers it) but not used for routing in this slice.
        cb.create_destination = [](const x1::DestinationDetails&) {
            return std::optional<x1::ErrorCode>{};
        };
        cb.remove_destination = [](const std::string&) { return std::optional<x1::ErrorCode>{}; };
        cb.remove_all_destinations = [] { return std::optional<x1::ErrorCode>{}; };
        return cb;
    }

    struct Job {
        std::string xid;
        std::vector<x1::TargetIdentifier> added;
        bool removed = false;
    };

    Config config;
    Hooks hooks;
    li_core::X2X3Client x2_client;

    mutable std::mutex store_mutex;
    std::unordered_map<std::string, TaskInfo> tasks;
    std::unordered_map<std::string, std::uint32_t> sequence;

    // LI_X1 listener: its own io_context on its own thread (the NF's main io_context and its
    // protocol threads are separate).
    boost::asio::io_context x1_ioc;
    std::unique_ptr<sbi_core::http2::Server> x1_server;
    std::thread x1_thread;

    x1::KeepaliveMonitor keepalive;
    std::mutex keepalive_mutex;
    std::thread keepalive_thread;
    std::atomic<bool> running{false};

    std::mutex job_mutex;
    std::condition_variable job_cv;
    std::deque<Job> jobs;
    std::thread worker_thread;
};

PoiRuntime::PoiRuntime(Config config, Hooks hooks)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(hooks))) {}

PoiRuntime::~PoiRuntime() {
    stop();
}

const Config& PoiRuntime::config() const {
    return impl_->config;
}

void PoiRuntime::start() {
    const sbi_core::http2::TlsConfig tls{
        .cert_path = impl_->config.cert_path,
        .key_path = impl_->config.key_path,
        .ca_path = impl_->config.ca_path,
    };
    impl_->x1_server = std::make_unique<sbi_core::http2::Server>(
        impl_->x1_ioc, impl_->config.x1_bind_address, impl_->config.x1_port, tls);
    impl_->x1_server->add_route(
        "POST",
        kX1Path,
        [this, callbacks = impl_->make_callbacks()](const sbi_core::http2::Request& request) {
            {
                const std::lock_guard<std::mutex> lock(impl_->keepalive_mutex);
                impl_->keepalive.on_x1_request(std::chrono::steady_clock::now());
            }
            sbi_core::http2::Response response;
            response.status = 200; // X1-level errors ride in the body, not the HTTP status
            response.headers.emplace("content-type", "application/xml");
            response.body = x1::handle_request(request.body, callbacks);
            return response;
        });
    impl_->x1_server->start();
    sbi_core::stop_on_shutdown_signal(impl_->x1_ioc);

    impl_->running.store(true);
    impl_->worker_thread = std::thread([this] { impl_->worker_loop(); });
    impl_->x1_thread = std::thread([this] { impl_->x1_ioc.run(); });

    impl_->keepalive_thread = std::thread([this] {
        const std::string& log = impl_->hooks.log_name;
        while (impl_->running.load()) {
            for (int i = 0; i < 10 && impl_->running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!impl_->running.load()) {
                break;
            }
            x1::KeepaliveMonitor::Action action{};
            {
                const std::lock_guard<std::mutex> lock(impl_->keepalive_mutex);
                action = impl_->keepalive.tick(std::chrono::steady_clock::now());
            }
            switch (action) {
                case x1::KeepaliveMonitor::Action::SendFaultReport:
                    spdlog::error(
                        "{}: no X1 request within TIME_P2 -- ReportNEIssue FaultReport is due "
                        "to the ADMF (needs an X1 client)",
                        log);
                    break;
                case x1::KeepaliveMonitor::Action::SendFaultCleared:
                    spdlog::info("{}: X1 contact restored", log);
                    break;
                case x1::KeepaliveMonitor::Action::DeactivateAllTasks:
                    spdlog::error("{}: TIME_P3 expired with no ADMF contact -- deactivating tasks",
                                  log);
                    impl_->deactivate_all();
                    break;
                case x1::KeepaliveMonitor::Action::None:
                    break;
            }
        }
    });

    spdlog::info("{}: LI_X1 on https://{}:{}{} (TLS 1.3 + mTLS), LI_X2 to MDF2 {}:{}",
                 impl_->hooks.log_name,
                 impl_->config.x1_bind_address,
                 impl_->config.x1_port,
                 kX1Path,
                 impl_->config.mdf2_host,
                 impl_->config.mdf2_port);
}

void PoiRuntime::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->x1_ioc.stop();
    {
        const std::lock_guard<std::mutex> lock(impl_->job_mutex); // pair with the wait predicate
    }
    impl_->job_cv.notify_all();
    if (impl_->worker_thread.joinable()) {
        impl_->worker_thread.join();
    }
    if (impl_->x1_thread.joinable()) {
        impl_->x1_thread.join();
    }
    if (impl_->keepalive_thread.joinable()) {
        impl_->keepalive_thread.join();
    }
    impl_->x2_client.disconnect();
}

std::vector<Match> PoiRuntime::matches(const std::string& identity,
                                       const std::vector<x1::TargetIdentifierKind>& kinds) const {
    const std::string wanted = impl_->hooks.normalise(identity);
    std::vector<Match> out;
    const std::lock_guard<std::mutex> lock(impl_->store_mutex);
    for (const auto& [xid, task] : impl_->tasks) {
        for (const auto& target : task.targets) {
            bool kind_ok = false;
            for (const auto k : kinds) {
                kind_ok = kind_ok || k == target.kind;
            }
            if (kind_ok && impl_->hooks.normalise(target.value) == wanted) {
                out.push_back(Match{xid, target, task.identifier_association});
                break;
            }
        }
    }
    return out;
}

std::optional<TaskInfo> PoiRuntime::task(const std::string& xid) const {
    const std::lock_guard<std::mutex> lock(impl_->store_mutex);
    const auto it = impl_->tasks.find(xid);
    if (it == impl_->tasks.end()) {
        return std::nullopt;
    }
    return it->second;
}

void PoiRuntime::emit(const Match& match,
                      std::span<const std::uint8_t> payload,
                      li_core::PayloadDirection direction,
                      const std::string& record) {
    const std::string& log = impl_->hooks.log_name;
    const auto xid_bytes = uuid_to_bytes(match.xid);
    if (!xid_bytes) {
        spdlog::warn("{}: task XID {} is not a UUID -- cannot build the X2 PDU", log, match.xid);
        return;
    }
    li_core::Pdu pdu;
    pdu.type = li_core::PduType::X2;
    pdu.payload_format = li_core::PayloadFormat::Tgpp33128Payload;
    pdu.payload_direction = direction;
    pdu.xid = *xid_bytes;
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    pdu.attributes.push_back(li_core::attr_sequence_number(impl_->next_sequence(match.xid)));
    pdu.attributes.push_back(li_core::attr_network_function_id(impl_->config.network_function_id));
    pdu.attributes.push_back(
        li_core::attr_interception_point_id(impl_->config.interception_point_id));
    pdu.attributes.push_back(li_core::attr_timestamp(now, 0));
    pdu.attributes.push_back(li_core::attr_matched_target_identifier(
        "<" + match.target.element + ">" + match.target.value + "</" + match.target.element + ">"));
    pdu.payload.assign(payload.begin(), payload.end());

    if (const auto sent = impl_->x2_client.send(pdu); !sent) {
        // Best-effort: an intercept delivery failure is logged, never allowed to break the
        // procedure on the network side.
        spdlog::error(
            "{}: LI_X2 delivery of an {} xIRI to the MDF2 failed: {}", log, record, sent.error());
        return;
    }
    spdlog::info("{}: delivered {} xIRI for target XID {}", log, record, match.xid);
}

void PoiRuntime::emit_cc(const std::string& xid,
                         li_core::PayloadFormat format,
                         li_core::PayloadDirection direction,
                         std::span<const std::uint8_t> packet) {
    const std::string& log = impl_->hooks.log_name;
    const auto xid_bytes = uuid_to_bytes(xid);
    if (!xid_bytes) {
        spdlog::warn("{}: task XID {} is not a UUID -- cannot build the X3 PDU", log, xid);
        return;
    }
    li_core::Pdu pdu;
    pdu.type = li_core::PduType::X3;
    pdu.payload_format = format;
    pdu.payload_direction = direction;
    pdu.xid = *xid_bytes;
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    pdu.attributes.push_back(li_core::attr_sequence_number(impl_->next_sequence(xid)));
    pdu.attributes.push_back(li_core::attr_network_function_id(impl_->config.network_function_id));
    pdu.attributes.push_back(
        li_core::attr_interception_point_id(impl_->config.interception_point_id));
    pdu.attributes.push_back(li_core::attr_timestamp(now, 0));
    pdu.payload.assign(packet.begin(), packet.end());
    if (const auto sent = impl_->x2_client.send(pdu); !sent) {
        spdlog::error(
            "{}: LI_X3 delivery of xCC for XID {} to the MDF3 failed: {}", log, xid, sent.error());
    }
}

} // namespace li_poi
