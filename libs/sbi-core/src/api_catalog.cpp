#include "sbi_core/api_catalog.hpp"

#include <algorithm>
#include <array>

namespace sbi_core::api_catalog {
namespace {

struct Row {
    std::string_view api_name;
    std::string_view scope;
    std::string_view nf_type;
};

constexpr auto kRows = std::to_array<Row>({
#include "api_catalog.inc"
});

} // namespace

std::optional<ApiInfo> find(std::string_view api_name) {
    const auto it = std::ranges::find(kRows, api_name, &Row::api_name);
    if (it == kRows.end()) {
        return std::nullopt;
    }
    return ApiInfo{.scope = it->scope, .nf_type = it->nf_type};
}

std::string api_name_of(std::string_view path_or_url) {
    if (const auto scheme = path_or_url.find("://"); scheme != std::string_view::npos) {
        const auto slash = path_or_url.find('/', scheme + 3);
        path_or_url =
            slash == std::string_view::npos ? std::string_view{} : path_or_url.substr(slash);
    }
    path_or_url = path_or_url.substr(0, path_or_url.find_first_of("?#"));
    const auto start = path_or_url.find_first_not_of('/');
    if (start == std::string_view::npos) {
        return {};
    }
    const auto end = path_or_url.find('/', start);
    return std::string(
        path_or_url.substr(start, end == std::string_view::npos ? end : end - start));
}

} // namespace sbi_core::api_catalog
