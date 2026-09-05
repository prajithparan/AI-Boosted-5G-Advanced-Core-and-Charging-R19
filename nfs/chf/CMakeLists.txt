# redis-plus-plus (Valkey-protocol-compatible): real persistence for CHF's stores (E3 Session
# Establishment's explicit "must be idempotent and recoverable across restarts and network
# partitions" requirement, docs/DATA_MODEL.md's own Redis/Valkey assignment) -- see
# docs/DECISIONS.md for the ADR this session added.
find_package(redis++ CONFIG REQUIRED)

# libmariadb (ADR-0192: real CDF/CDR generation on Apache Doris -- Doris speaks the real MySQL
# wire protocol). Deliberately the plain C client library, NOT
# mariadb-connector-cpp: that C++ wrapper's own API surface (sql::Driver/Connection/
# PreparedStatement/ResultSet/DriverManager::getConnection) mirrors JDBC's class-naming
# convention, which is a standing, explicit project instruction to avoid regardless of the
# library's own real license/dependency chain (mariadb-connector-cpp has zero actual Java/JVM
# dependency -- it was rejected for its API shape, not for being Java). cdr.hpp/cdr.cpp instead
# wrap the plain C API (MYSQL*, mysql_real_connect, mysql_real_query, ...) directly in a small,
# project-owned RAII class with no JDBC-shaped naming. Real target name confirmed by reading the
# port's own generated unofficial-libmariadb-config.cmake directly: unofficial::libmariadb.
find_package(unofficial-libmariadb CONFIG REQUIRED)

# libpqxx (P4.5/ADR-0060: E5 RatingDecision audit table, docs/DATA_MODEL.md's PostgreSQL
# assignment) -- CHF's first real PostgreSQL connection.
find_package(libpqxx CONFIG REQUIRED)

# onnxruntime (P4.8/ADR-0074: real in-process C++ inference for predictive quota sizing).
# find_package(onnxruntime CONFIG) genuinely works for this port -- confirmed by successful
# compilation. Real, disclosed static-linking bug found via this ADR's own
# unit tests (tests/conformance/test_ai_inference.cpp): a plain static link of ONNX::onnx (pulled
# in transitively by onnxruntime::onnxruntime) drops most of its operator-schema
# self-registration object files, since nothing in this executable directly references their
# symbols (ar-style static archives only pull in .o files that resolve an undefined symbol) --
# ONNX Runtime then fails to load ANY real model with "SchemasRegisterer" assertion failures ("4
# schema were exposed... 741 were expected"). Fixed with CMake's portable whole-archive link-library
# generator expression, forcing every object file in libonnx.a into the final binary regardless of
# whether anything references it directly -- the standard real fix for self-registering-global
# static libraries, not a workaround specific to this project.
find_package(onnxruntime CONFIG REQUIRED)
find_package(ONNX CONFIG REQUIRED)

add_executable(chf src/main.cpp src/stores.cpp src/cdr.cpp src/cdr_asn1.cpp src/diameter_server.cpp src/rating_decision_store.cpp src/charging_engine.cpp src/proportional_debit.cpp src/cap_server.cpp src/ai_inference.cpp)
target_link_libraries(chf PRIVATE sbi_core sbi_generated bss_sid redis++::redis++_static unofficial::libmariadb diameter_core libpqxx::pqxx ss7_core tcap_core cap_core tbcd_core onnxruntime::onnxruntime "$<LINK_LIBRARY:WHOLE_ARCHIVE,ONNX::onnx>" nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/chf.json (partial retrofit
# this turn, see main.cpp's own comment for what's deliberately still deferred).
target_compile_definitions(chf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
