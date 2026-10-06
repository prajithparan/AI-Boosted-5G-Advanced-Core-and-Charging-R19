# The warrant store shared by the MDF2 (nfs/li-mdf) and the MDF3 (nfs/li-mdf3): the X1-provisioned
# tasks, Destination IDs and per-LIID HI sequence counters, in Valkey (ADR-0377, ADR-0464).
find_package(redis++ CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(li_mdf_store STATIC src/task_store.cpp)
target_include_directories(li_mdf_store PUBLIC include)
target_link_libraries(li_mdf_store PUBLIC li_core redis++::redis++_static nlohmann_json::nlohmann_json)
target_compile_features(li_mdf_store PUBLIC cxx_std_23)
