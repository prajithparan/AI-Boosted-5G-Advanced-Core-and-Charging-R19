// Independent check of the GUI's derived TMF620 schema (ADR-0421) against the REAL serializers.
//
// derive_tmf620_schema.py reads product.hpp's struct declarations; this test does not trust that
// parser. It builds a maximal instance from the emitted schema -- every property populated, every
// array with exactly one element (empty arrays are omitted by to_json, which would hide a wrong
// key), non-integral numbers -- and pushes it through bss_sid::from_json -> to_json. Equality
// proves that every key and type in the schema is one the DTO really reads and writes, and that
// the DTO writes nothing the schema lacks. Then each `required` key is removed in turn (from_json
// must throw) and each optional key likewise (it must not).

#include "bss_sid/product.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <algorithm>
#include <functional>
#include <set>
#include <string>

#ifndef TMF620_SCHEMA_PATH
#error "TMF620_SCHEMA_PATH must be defined by CMake"
#endif

namespace {

using nlohmann::json;

json load_schema() {
    std::ifstream in(TMF620_SCHEMA_PATH);
    return json::parse(in);
}

json sample(const json& schema, const std::string& path) {
    const std::string type = schema.value("type", "");
    if (type == "object") {
        json out = json::object();
        for (const auto& [k, sub] : schema.at("properties").items()) {
            out[k] = sample(sub, path + "." + k);
        }
        return out;
    }
    if (type == "array") {
        return json::array({sample(schema.at("items"), path + "[]")});
    }
    if (type == "string") {
        // A date-time-formatted field gets a date-time; the rest a value naming its own path,
        // so a swapped pair of keys cannot round-trip equal.
        return schema.value("format", "") == "date-time" ? json("2026-09-25T10:00:00.000Z")
                                                         : json("v" + path);
    }
    if (type == "integer") return 7;
    if (type == "number") return 1.5;
    if (type == "boolean") return true;
    // Untyped JSON in the DTO: any value must pass through unchanged.
    return json{{"@type", "Opaque"}, {"n", 3}};
}

template <typename T> json round_trip(const json& in) {
    T dto = in.get<T>();
    return json(dto);
}

template <typename T> void check_resource(const char* name) {
    const auto schema = load_schema().at("definitions").at(name);
    const json in = sample(schema, name);
    EXPECT_EQ(round_trip<T>(in), in) << name << ": the schema and the DTO disagree";
}

TEST(Tmf620SchemaRoundTrip, ProductOfferingMatchesTheDto) {
    check_resource<bss_sid::ProductOffering>("ProductOffering");
}

TEST(Tmf620SchemaRoundTrip, ProductOfferingPriceMatchesTheDto) {
    check_resource<bss_sid::ProductOfferingPrice>("ProductOfferingPrice");
}

// Walks every object sub-schema; for each property, removes it from the sample at that spot and
// checks from_json's reaction matches the schema's `required` -- except where `required` comes
// from the service overlay (name on create), which the DTO itself does not enforce.
template <typename T> void check_required(const char* name, const std::set<std::string>& overlay) {
    const auto schema = load_schema().at("definitions").at(name);
    const json full = sample(schema, name);
    int checked = 0;
    std::function<void(const json&, const json::json_pointer&)> walk =
        [&](const json& s, const json::json_pointer& ptr) {
            if (s.value("type", "") == "array") {
                walk(s.at("items"), ptr / 0);
                return;
            }
            if (s.value("type", "") != "object") return;
            const auto req = s.value("required", json::array());
            for (const auto& [k, sub] : s.at("properties").items()) {
                json cut = full;
                cut.at(ptr).erase(k);
                const bool is_required =
                    std::find(req.begin(), req.end(), k) != req.end() &&
                    !(ptr.empty() && overlay.count(k) != 0);
                bool threw = false;
                try {
                    (void)cut.get<T>();
                } catch (const json::exception&) {
                    threw = true;
                }
                EXPECT_EQ(threw, is_required) << name << ptr.to_string() << "/" << k;
                ++checked;
                walk(sub, ptr / k);
            }
        };
    walk(schema, json::json_pointer());
    EXPECT_GT(checked, 50) << "the walk must actually cover the nested schema";
}

TEST(Tmf620SchemaRoundTrip, RequiredMatchesWhatFromJsonEnforces) {
    check_required<bss_sid::ProductOffering>("ProductOffering", {"name"});
    check_required<bss_sid::ProductOfferingPrice>("ProductOfferingPrice", {});
}

} // namespace
