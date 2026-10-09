# nas-alg-provider (ADR-0479): the fail-closed loader for an operator-supplied NAS algorithm
# provider. Contains no algorithm and no restricted (SAGE) material.
find_package(OpenSSL REQUIRED)
find_package(tl-expected CONFIG REQUIRED)

add_library(nas_alg_provider STATIC src/loader.cpp)
target_include_directories(nas_alg_provider PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(nas_alg_provider PUBLIC 5gc_project_options tl::expected OpenSSL::Crypto ${CMAKE_DL_LIBS})
target_compile_features(nas_alg_provider PUBLIC cxx_std_23)

# Loader tests: a TEST-ONLY dummy provider (not SNOW 3G) exercises every fail-closed gate.
find_package(GTest CONFIG REQUIRED)
add_library(nasalg_test_provider MODULE test/test_provider.c)
target_include_directories(nasalg_test_provider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(nasalg_test_provider PROPERTIES C_STANDARD 11)
add_executable(nas_alg_provider_tests test/test_loader.cpp)
target_link_libraries(nas_alg_provider_tests PRIVATE nas_alg_provider GTest::gtest GTest::gtest_main)
target_compile_definitions(nas_alg_provider_tests PRIVATE
    TEST_PROVIDER_PATH="$<TARGET_FILE:nasalg_test_provider>")
add_dependencies(nas_alg_provider_tests nasalg_test_provider)
include(GoogleTest)
gtest_discover_tests(nas_alg_provider_tests PROPERTIES TIMEOUT 60)
