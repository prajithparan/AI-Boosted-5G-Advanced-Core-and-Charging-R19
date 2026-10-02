#include "catalog_snapshot.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <unordered_map>

#include "charging_engine.hpp"

namespace chf {

namespace {
using nlohmann::json;
}

bool CatalogSnapshot::refresh(sbi_core::http2::Client& catalog_client) {
    sbi_core::http2::ClientRequest offerings_req;
    offerings_req.method = "GET";
    offerings_req.url = product_catalog_base() + kProductCatalogApiRoot + "/productOffering";
    auto offerings_resp = catalog_client.send(offerings_req);
    if (!offerings_resp.has_value() || offerings_resp->status != 200) {
        spdlog::warn("chf: catalog snapshot refresh could not reach product-catalog for "
                     "productOffering, keeping previous snapshot");
        return false;
    }

    sbi_core::http2::ClientRequest prices_req;
    prices_req.method = "GET";
    prices_req.url = product_catalog_base() + kProductCatalogApiRoot + "/productOfferingPrice";
    auto prices_resp = catalog_client.send(prices_req);
    if (!prices_resp.has_value() || prices_resp->status != 200) {
        spdlog::warn("chf: catalog snapshot refresh could not reach product-catalog for "
                     "productOfferingPrice, keeping previous snapshot");
        return false;
    }

    std::vector<bss_sid::ProductOffering> offerings;
    std::vector<bss_sid::ProductOfferingPrice> prices;
    try {
        offerings = json::parse(offerings_resp->body).get<std::vector<bss_sid::ProductOffering>>();
        prices = json::parse(prices_resp->body).get<std::vector<bss_sid::ProductOfferingPrice>>();
    } catch (const json::exception& e) {
        spdlog::warn("chf: catalog snapshot refresh got malformed JSON from product-catalog: {}, "
                     "keeping previous snapshot",
                     e.what());
        return false;
    }

    std::unordered_map<std::string, const bss_sid::ProductOfferingPrice*> price_by_id;
    price_by_id.reserve(prices.size());
    for (const auto& price : prices) {
        if (price.id.has_value()) {
            price_by_id.emplace(*price.id, &price);
        }
    }

    // Same order as the real GET /productOffering response -- "first match wins" (the decoy/
    // wanted-offering ordering test in tests/integration/test_cap_scoped_charging.cpp) depends on
    // this, so it is preserved exactly rather than re-sorted or grouped by rating group.
    std::vector<CatalogCandidate> new_candidates;
    new_candidates.reserve(offerings.size());
    for (const auto& offering : offerings) {
        // Mirrors the live-fetch path's own "no price ref at all" skip (charging_engine.cpp) --
        // only the FIRST price ref is ever consulted there, so only it is cached here.
        if (offering.productOfferingPrice.empty()) {
            continue;
        }
        const auto it = price_by_id.find(offering.productOfferingPrice.front().id);
        if (it == price_by_id.end()) {
            // Mirrors the live-fetch path's own "price GET returned non-200/malformed" skip.
            continue;
        }
        new_candidates.push_back(CatalogCandidate{offering, *it->second});
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        candidates_ = std::move(new_candidates);
    }
    ready_.store(true, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    last_refresh_unix_ms_.store(now_ms, std::memory_order_release);
    return true;
}

double CatalogSnapshot::age_seconds() const {
    const auto last_ms = last_refresh_unix_ms_.load(std::memory_order_acquire);
    if (last_ms < 0) {
        return -1.0;
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return static_cast<double>(now_ms - last_ms) / 1000.0;
}

std::vector<CatalogCandidate> CatalogSnapshot::candidates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return candidates_;
}

} // namespace chf
