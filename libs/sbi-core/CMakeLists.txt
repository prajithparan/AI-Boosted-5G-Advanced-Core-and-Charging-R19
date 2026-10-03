# nghttp2's vcpkg port ships only a pkg-config file (libnghttp2.pc), no CMake config -- see
# docs/DECISIONS.md ADR-0004.
find_package(PkgConfig REQUIRED)
pkg_check_modules(nghttp2 REQUIRED IMPORTED_TARGET libnghttp2)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(opentelemetry-cpp CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)

add_library(sbi_core STATIC
    src/problem_details.cpp
    src/sbi_headers.cpp
    src/uuid.cpp
    src/datetime.cpp
    src/logging.cpp
    src/otel.cpp
    src/metrics.cpp
    src/http2_server.cpp
    src/http2_client.cpp
    src/io_context_pool.cpp
    src/oauth2_client.cpp
    src/jwt.cpp
    src/multipart.cpp
    src/json_body.cpp
    src/rate_limit.cpp
)

target_include_directories(sbi_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(sbi_core PUBLIC
    5gc_project_options
    PkgConfig::nghttp2
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    CURL::libcurl
    opentelemetry-cpp::trace
    opentelemetry-cpp::metrics
    opentelemetry-cpp::ostream_span_exporter
    opentelemetry-cpp::otlp_http_exporter
    opentelemetry-cpp::prometheus_exporter
    tl::expected
    jwt-cpp::jwt-cpp
    nf_config # ADR-0449: PoolExhaustedError, caught in http2_server.cpp's handler boundary
)

target_compile_features(sbi_core PUBLIC cxx_std_23)
