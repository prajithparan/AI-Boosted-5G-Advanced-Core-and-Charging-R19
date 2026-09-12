#include "sbi_core/sbi_headers.hpp"

#include <array>
#include <format>
#include <sstream>

namespace sbi_core::headers {

namespace {

constexpr std::array<const char*, 7> kDayNames = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<const char*, 12> kMonthNames = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

} // namespace

std::string format_sender_timestamp(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;

    const auto ms = floor<milliseconds>(tp);
    const auto days_tp = floor<days>(ms);
    const year_month_day ymd{days_tp};
    const weekday wd{days_tp};
    const hh_mm_ss<milliseconds> hms{ms - days_tp};

    return std::format(
        "{}, {:02} {} {} {:02}:{:02}:{:02}.{:03} GMT",
        kDayNames.at(static_cast<std::size_t>(wd.c_encoding())),
        static_cast<unsigned>(ymd.day()),
        kMonthNames.at(static_cast<std::size_t>(static_cast<unsigned>(ymd.month())) - 1),
        static_cast<int>(ymd.year()),
        hms.hours().count(),
        hms.minutes().count(),
        hms.seconds().count(),
        hms.subseconds().count());
}

std::string format_producer_id(const ProducerId& id) {
    std::string out = std::format("nfinst={}", id.nf_instance_id);
    if (id.nf_service_instance_id) {
        out += std::format(";nfservinst={}", *id.nf_service_instance_id);
    }
    if (id.nf_set_id) {
        out += std::format(";nfset={}", *id.nf_set_id);
    }
    if (id.nf_service_set_id) {
        out += std::format(";nfserviceset={}", *id.nf_service_set_id);
    }
    return out;
}

std::optional<ProducerId> parse_producer_id(const std::string& header_value) {
    ProducerId id;
    bool found_nfinst = false;

    std::stringstream ss(header_value);
    std::string part;
    while (std::getline(ss, part, ';')) {
        const auto eq = part.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = part.substr(0, eq);
        const std::string value = part.substr(eq + 1);
        if (key == "nfinst") {
            id.nf_instance_id = value;
            found_nfinst = true;
        } else if (key == "nfservinst") {
            id.nf_service_instance_id = value;
        } else if (key == "nfset") {
            id.nf_set_id = value;
        } else if (key == "nfserviceset") {
            id.nf_service_set_id = value;
        }
    }

    if (!found_nfinst) {
        return std::nullopt;
    }
    return id;
}

std::map<std::string, std::string> parse_request_info(const std::string& header_value) {
    std::map<std::string, std::string> params;
    // The ABNF permits OWS after "=" and after each ";", which parse_producer_id above does not
    // have to handle because its own header has none. Trimmed here rather than left in the value,
    // where it would make an idempotency key compare unequal to the same key sent on the retry.
    const auto trim = [](std::string v) {
        const auto first = v.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return std::string{};
        }
        const auto last = v.find_last_not_of(" \t");
        return v.substr(first, last - first + 1);
    };

    std::stringstream ss(header_value);
    std::string part;
    while (std::getline(ss, part, ';')) {
        const auto eq = part.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto key = trim(part.substr(0, eq));
        auto value = trim(part.substr(eq + 1));
        if (key.empty()) {
            continue;
        }
        // req-param-value is a token; a quoted form appears in the spec's own callback-uri-prefix
        // example, so quotes are stripped rather than kept as part of the value.
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        params.emplace(std::move(key), std::move(value));
    }
    return params;
}

std::optional<std::string> request_info_idempotency_key(const std::string& header_value) {
    const auto params = parse_request_info(header_value);
    const auto it = params.find("idempotency-key");
    if (it == params.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace sbi_core::headers
