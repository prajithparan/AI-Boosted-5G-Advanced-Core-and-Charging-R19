cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(5gc-r19
    VERSION 0.1.0
    DESCRIPTION "5G Advanced Core and Charging R-19"
    LANGUAGES CXX C
)
# C is needed only for libs/ngap-generated's asn1c output (ADR-0030) -- every NF/lib's own code
# remains C++23; this doesn't relax that.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

option(5GC_ENABLE_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(5GC_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(5GC_BUILD_TESTS "Build tests" ON)
option(5GC_ENABLE_FUZZING "Build libFuzzer harnesses for codec decode paths (tests/fuzz/) -- requires clang" OFF)

add_library(5gc_project_options INTERFACE)
target_compile_options(5gc_project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
)

if(5GC_ENABLE_ASAN AND 5GC_ENABLE_TSAN)
    message(FATAL_ERROR "5GC_ENABLE_ASAN and 5GC_ENABLE_TSAN are mutually exclusive")
endif()

if(5GC_ENABLE_ASAN)
    target_compile_options(5gc_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(5gc_project_options INTERFACE -fsanitize=address,undefined)
endif()

if(5GC_ENABLE_TSAN)
    target_compile_options(5gc_project_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(5gc_project_options INTERFACE -fsanitize=thread)
endif()

if(5GC_ENABLE_FUZZING)
    if(5GC_ENABLE_ASAN OR 5GC_ENABLE_TSAN)
        message(FATAL_ERROR "5GC_ENABLE_FUZZING is mutually exclusive with 5GC_ENABLE_ASAN/5GC_ENABLE_TSAN -- libFuzzer harnesses carry their own -fsanitize=fuzzer,address")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "5GC_ENABLE_FUZZING requires Clang (libFuzzer is a Clang/LLVM-only sanitizer, see tests/fuzz/CMakeLists.txt)")
    endif()
endif()

include(CTest)

add_subdirectory(libs/sbi-core)
add_subdirectory(libs/sbi-generated)
add_subdirectory(libs/aka-crypto)
add_subdirectory(libs/ngap-generated)
add_subdirectory(libs/ngap-core)
add_subdirectory(libs/pfcp-core)
add_subdirectory(libs/bss-sid)
add_subdirectory(libs/diameter-core)
add_subdirectory(libs/ss7-core)
add_subdirectory(libs/tcap-core)
add_subdirectory(libs/cap-core)
add_subdirectory(libs/map-core)
add_subdirectory(libs/tbcd-core)
add_subdirectory(libs/tap3-core)
add_subdirectory(libs/nf-config)
add_subdirectory(nfs/nrf)
add_subdirectory(nfs/amf)
add_subdirectory(nfs/smf)
add_subdirectory(nfs/udm)
add_subdirectory(nfs/udr)
add_subdirectory(nfs/ausf)
add_subdirectory(nfs/pcf)
add_subdirectory(nfs/nssf)
add_subdirectory(nfs/bsf)
add_subdirectory(nfs/nef)
add_subdirectory(nfs/scp)
add_subdirectory(nfs/eir)
add_subdirectory(nfs/smsf)
add_subdirectory(nfs/gmlc)
add_subdirectory(nfs/lmf)
add_subdirectory(nfs/nsacf)
add_subdirectory(nfs/upf)
add_subdirectory(nfs/chf)
add_subdirectory(nfs/hello-nf)
add_subdirectory(bss/product-catalog)
add_subdirectory(bss/balance-management)
add_subdirectory(bss/subscriber-management)
add_subdirectory(bss/roaming-interconnect)
add_subdirectory(tools/sbi-loadgen)
add_subdirectory(tools/mcp-server)

if(5GC_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests/integration)
    add_subdirectory(tests/conformance)
endif()

if(5GC_ENABLE_FUZZING)
    add_subdirectory(tests/fuzz)
endif()
