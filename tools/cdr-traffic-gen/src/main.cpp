// ADR-0337: a charging-traffic generator that produces REAL CDRs.
//
// ADR-0336 raised the training threshold to 2000 examples over 200 subscribers and measured what
// the lab actually had: 12 usable training pairs. No model, open-source or otherwise, fixes that --
// nothing pretrained knows this network's subscribers or tariffs. The only thing that unblocks the
// charging models is volume, and this is what produces it.
//
// THE DESIGN DECISION THAT MATTERS: this drives CHF's real N40 interface -- Create, Update,
// Release, over HTTP/2 + mTLS, through the real rating engine, the real balance reservation and
// the real CDR writer. It does NOT insert rows into Doris.
//
// Inserting rows would be faster and completely worthless. Those CDRs would carry whatever values
// this generator chose, so a model trained on them would learn this file's arithmetic rather than
// the charging system's behaviour -- while being tagged `data_source=real_cdr` because they came
// out of the real table. That is precisely the mislabelling ADR-0336 exists to prevent, arrived at
// from the other direction.
//
// What is synthetic here is the OFFERED LOAD: which subscriber, when, how much usage. That is
// honest and unavoidable -- a lab has no real subscribers. What is real is every charging decision
// made about that load.

#include "sbi_core/datetime.hpp"
#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using nlohmann::json;

struct Options {
    // ADR-0339: repeatable. CHF's CDR writer serializes on one mutex and one Doris connection,
    // so a SECOND CHF process -- not a second load thread -- is what actually adds write
    // throughput. Safe to fork: ChargingDataRef comes from a Redis INCR, an atomic shared
    // counter, so instances cannot collide on refs.
    std::vector<std::string> chf_bases;
    std::string cert, key, ca;
    int subscribers = 10000;
    int sessions = 1000000;
    int concurrency = 8;
    int rating_groups = 3;
    int updates = 2;
    std::uint64_t seed = 1;
};

struct Stats {
    std::atomic<std::uint64_t> created{0};
    std::atomic<std::uint64_t> released{0};
    // Usage-bearing Update CDRs: the only rows a usage model can train on.
    std::atomic<std::uint64_t> usage_reported{0};
    std::atomic<std::uint64_t> failed{0};
};

void print_usage() {
    std::cout << R"(cdr-traffic-gen -- drives CHF's real N40 path to accumulate genuine CDRs

  --chf <url>            CHF base URL. REPEATABLE: give one per CHF instance and load is spread
                         across them. Adding CHF processes is what raises write throughput;
                         adding load threads against one CHF does not, because its CDR writer
                         serializes on a single mutex and connection.
  --cert/--key/--ca      mTLS material (required)
  --subscribers <n>      distinct SUPIs to spread load across (default 10000)
  --sessions <n>         charging sessions to run; one CDR per Release (default 1000000)
  --concurrency <n>      worker threads (default 8)
  --rating-groups <n>    distinct rating groups per subscriber (default 3)
  --updates <n>          usage-reporting Updates per session (default 2). THIS is what produces
                         trainable rows: a CDR is only usable for usage modelling when it carries
                         BOTH rating_group and used_total_volume, and only an Update does.
  --seed <n>             RNG seed, so a run is reproducible

One session = Create + Update + Release against the real charging engine. The CDR is written by
CHF at Release, not by this tool.
)";
}

std::string supi_for(int index) {
    // Zero-padded so the SUPI space is uniform and sorts predictably in the CDR table.
    std::string n = std::to_string(index);
    return "imsi-99970" + std::string(10 - n.size(), '0') + n;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else if (a == "--chf") {
            opt.chf_bases.push_back(next("--chf"));
        } else if (a == "--cert") {
            opt.cert = next("--cert");
        } else if (a == "--key") {
            opt.key = next("--key");
        } else if (a == "--ca") {
            opt.ca = next("--ca");
        } else if (a == "--subscribers") {
            opt.subscribers = std::stoi(next("--subscribers"));
        } else if (a == "--sessions") {
            opt.sessions = std::stoi(next("--sessions"));
        } else if (a == "--concurrency") {
            opt.concurrency = std::stoi(next("--concurrency"));
        } else if (a == "--rating-groups") {
            opt.rating_groups = std::stoi(next("--rating-groups"));
        } else if (a == "--updates") {
            opt.updates = std::stoi(next("--updates"));
        } else if (a == "--seed") {
            opt.seed = std::stoull(next("--seed"));
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return 2;
        }
    }
    if (opt.chf_bases.empty()) {
        opt.chf_bases.push_back("https://127.0.0.1:7784");
    }
    if (opt.cert.empty() || opt.key.empty() || opt.ca.empty()) {
        std::cerr << "--cert, --key and --ca are required\n";
        return 2;
    }

    Stats stats;
    std::atomic<int> next_session{0};
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(opt.concurrency));
    for (int w = 0; w < opt.concurrency; ++w) {
        workers.emplace_back([&, w] {
            sbi_core::http2::TlsConfig tls{
                .cert_path = opt.cert, .key_path = opt.key, .ca_path = opt.ca};
            sbi_core::http2::Client client(std::move(tls));
            // Per-worker RNG, seeded from the shared seed plus the worker index: reproducible as a
            // whole while never producing the same stream in two threads.
            std::mt19937_64 rng(opt.seed + static_cast<std::uint64_t>(w));
            std::uniform_int_distribution<int> sub_pick(0, opt.subscribers - 1);
            std::uniform_int_distribution<int> rg_pick(1, opt.rating_groups);
            // A realistic-ish spread rather than a constant: a model trained on identical usage
            // learns a constant. Lognormal because real usage is heavily right-skewed -- most
            // sessions small, a few very large.
            std::lognormal_distribution<double> volume(15.0, 1.5);

            // Each worker is pinned to one CHF instance rather than round-robining per request:
            // a session's Create, Updates and Release must all reach the instance holding it --
            // the session state is in shared Redis, but keeping a session on one connection avoids
            // paying a fresh TLS handshake per hop.
            const std::string& base =
                opt.chf_bases[static_cast<std::size_t>(w) % opt.chf_bases.size()];
            const std::string create_url = base + "/nchf-convergedcharging/v3/chargingdata";

            auto post = [&](const std::string& url,
                            const json& body) -> std::pair<int, std::string> {
                sbi_core::http2::ClientRequest req;
                req.method = "POST";
                req.url = url;
                req.headers.emplace("content-type", "application/json");
                req.body = body.dump();
                auto resp = client.send(req);
                if (!resp.has_value()) {
                    return {-1, {}};
                }
                std::string location;
                if (const auto it = resp->headers.find("location"); it != resp->headers.end()) {
                    location = it->second;
                }
                return {resp->status, location.empty() ? resp->body : location};
            };

            while (true) {
                const int n = next_session.fetch_add(1);
                if (n >= opt.sessions) {
                    break;
                }
                const auto supi = supi_for(sub_pick(rng));
                const int rg = rg_pick(rng);
                const auto used = static_cast<std::uint64_t>(volume(rng));

                json create{
                    {"nfConsumerIdentification", json{{"nodeFunctionality", "SMF"}}},
                    {"invocationTimeStamp",
                     sbi_core::format_rfc3339(std::chrono::system_clock::now())},
                    {"invocationSequenceNumber", 1},
                    {"subscriberIdentifier", supi},
                    {"multipleUnitUsage", json::array({json{{"ratingGroup", rg}}})},
                };
                auto [status, loc] = post(create_url, create);
                if (status != 201 || loc.empty()) {
                    stats.failed.fetch_add(1);
                    continue;
                }
                stats.created.fetch_add(1);
                const auto ref = loc.substr(loc.rfind('/') + 1);

                // Usage is reported in UPDATEs, not only in the Release -- which is both what
                // a real SMF does and the only thing that produces a trainable row.
                //
                // Measured the hard way: the first version reported usage solely in the Release,
                // and every Release CDR came back with rating_group NULL and used_total_volume
                // NULL. The training query needs BOTH non-null, so 16,000 CDRs yielded zero usable
                // examples. The run looked productive by row count and taught nothing.
                for (int u = 0; u < opt.updates; ++u) {
                    const auto chunk = static_cast<std::uint64_t>(volume(rng));
                    json update{
                        {"nfConsumerIdentification", json{{"nodeFunctionality", "SMF"}}},
                        {"invocationTimeStamp",
                         sbi_core::format_rfc3339(std::chrono::system_clock::now())},
                        {"invocationSequenceNumber", 2 + u},
                        {"subscriberIdentifier", supi},
                        {"multipleUnitUsage",
                         json::array({json{{"ratingGroup", rg},
                                           {"usedUnitContainer",
                                            json::array({json{{"localSequenceNumber", 1 + u},
                                                              {"totalVolume", chunk},
                                                              {"quotaManagementIndicator",
                                                               "NORMAL_QUOTA_CONSUMPTION"}}})}}})},
                    };
                    auto [ustatus, ubody] =
                        post(base + "/nchf-convergedcharging/v3/chargingdata/" + ref + "/update",
                             update);
                    if (ustatus >= 200 && ustatus < 300) {
                        stats.usage_reported.fetch_add(1);
                    } else {
                        stats.failed.fetch_add(1);
                    }
                }

                json release{
                    {"nfConsumerIdentification", json{{"nodeFunctionality", "SMF"}}},
                    {"invocationTimeStamp",
                     sbi_core::format_rfc3339(std::chrono::system_clock::now())},
                    {"invocationSequenceNumber", 2 + opt.updates},
                    {"subscriberIdentifier", supi},
                    {"multipleUnitUsage",
                     json::array({json{{"ratingGroup", rg},
                                       {"usedUnitContainer",
                                        json::array({json{{"localSequenceNumber", 1 + opt.updates},
                                                          {"totalVolume", used},
                                                          {"quotaManagementIndicator",
                                                           "NORMAL_QUOTA_CONSUMPTION"}}})}}})},
                };
                auto [rstatus, rbody] = post(
                    base + "/nchf-convergedcharging/v3/chargingdata/" + ref + "/release", release);
                if (rstatus >= 200 && rstatus < 300) {
                    stats.released.fetch_add(1);
                } else {
                    stats.failed.fetch_add(1);
                }
            }
        });
    }

    // Progress on stderr so stdout stays clean for a summary a script can parse.
    std::thread reporter([&] {
        while (next_session.load() < opt.sessions) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const auto done = stats.released.load();
            std::cerr << "[cdr-traffic-gen] released=" << done << " failed=" << stats.failed.load()
                      << " rate=" << (elapsed > 0 ? done / elapsed : 0.0) << "/s\n";
        }
    });

    for (auto& t : workers) {
        t.join();
    }
    reporter.join();

    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "sessions requested : " << opt.sessions << "\n"
              << "created            : " << stats.created.load() << "\n"
              << "released (CDRs)    : " << stats.released.load() << "\n"
              << "usage-bearing CDRs : " << stats.usage_reported.load()
              << "   <- the trainable ones\n"
              << "failed             : " << stats.failed.load() << "\n"
              << "elapsed seconds    : " << elapsed << "\n"
              << "CDRs per second    : " << (elapsed > 0 ? stats.released.load() / elapsed : 0.0)
              << "\n";
    // A run that produced no CDRs is a failed run, and must not exit 0 into a pipeline that then
    // trains on nothing.
    return stats.released.load() > 0 ? 0 : 1;
}
