#pragma once

#include <optional>
#include <string>
#include <string_view>

// What the REL-19 YAMLs say about each SBI API, keyed by its apiName ({apiRoot}/<apiName>/<ver>):
// the service-level OAuth2 scope and the producer NF type (ADR-0499). Generated into
// src/api_catalog.inc by tools/sbi-codegen/gen_api_catalog.py; see that script for how each
// column is derived and which APIs carry no NF type.
namespace sbi_core::api_catalog {

struct ApiInfo {
    std::string_view scope;
    std::string_view nf_type; // empty when the YAML's service name is not an NFType value
};

std::optional<ApiInfo> find(std::string_view api_name);

// The apiName of a request path or URL ("https://host/nudm-sdm/v2/..." -> "nudm-sdm"); empty when
// the path has no segment.
std::string api_name_of(std::string_view path_or_url);

} // namespace sbi_core::api_catalog
