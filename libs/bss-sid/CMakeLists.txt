# TM Forum SID -> Open API mapping layer (docs/CHARGING_MAPPING.md). Deliberately independent of
# sbi_core -- see include/bss_sid/party.hpp's file header for why (ODA component boundary,
# CLAUDE.md's "the BSS layer could be swapped for a commercial stack" goal).

find_package(nlohmann_json CONFIG REQUIRED)

add_library(bss_sid STATIC
    src/party.cpp
    src/product.cpp
    src/balance.cpp
    src/rating.cpp
    src/agreement.cpp
    src/money_exact.cpp
)

target_include_directories(bss_sid PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(bss_sid PUBLIC
    5gc_project_options
    nlohmann_json::nlohmann_json
)

target_compile_features(bss_sid PUBLIC cxx_std_23)
