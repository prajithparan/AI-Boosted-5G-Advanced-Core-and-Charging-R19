#include "sbi_core/json_body.hpp"

#include <set>
#include <string>
#include <vector>

namespace sbi_core::http2 {

Response problem_response(int status, const std::string& title, const std::string& detail) {
    auto pd = sbi_core::make_problem_details(status, title, detail);
    nlohmann::json j = pd;
    Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = j.dump();
    return r;
}

namespace {

// SAX handler: one name set per open object; a repeated name sets `duplicate` and stops the parse.
struct DuplicateKeyDetector : nlohmann::json::json_sax_t {
    std::vector<std::set<std::string>> objects;
    bool duplicate = false;

    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool start_object(std::size_t) override {
        objects.emplace_back();
        return true;
    }
    bool key(string_t& name) override {
        if (!objects.back().insert(name).second) {
            duplicate = true;
            return false; // stop
        }
        return true;
    }
    bool end_object() override {
        objects.pop_back();
        return true;
    }
    bool start_array(std::size_t) override { return true; }
    bool end_array() override { return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::json::exception&) override {
        return false;
    }
};

} // namespace

bool json_has_duplicate_keys(std::string_view body) {
    DuplicateKeyDetector detector;
    const std::string text(body);
    nlohmann::json::sax_parse(text, &detector);
    return detector.duplicate;
}

} // namespace sbi_core::http2
