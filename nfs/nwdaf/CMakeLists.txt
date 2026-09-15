# NWDAF -- TS 23.288 / TS 29.520, Phase A (ADR-0358) + Phase C (ADR-0368) + the MTLF (ADR-0369)
# + ML model accuracy monitoring (ADR-0370). Five of ten Nnwdaf services; see src/main.cpp.
# libmariadb: NWDAF reads the CHF feature store in Doris over the MySQL protocol -- its own reader
# (src/feature_store.cpp), not CHF's, since no NF includes another's private headers.
find_package(unofficial-libmariadb CONFIG REQUIRED)
# redis-plus-plus (Valkey-protocol-compatible): subscription state shared across replicas (ADR-0360).
find_package(redis++ CONFIG REQUIRED)
# onnxruntime (ADR-0369): in-process inference with the MTLF-provisioned model in the AnLF
# (src/model_runtime.cpp). Same link recipe as nfs/chf/CMakeLists.txt, for the same reason: a
# plain static link of ONNX::onnx drops operator schemas the runtime needs at session creation.
find_package(onnxruntime CONFIG REQUIRED)

add_executable(nwdaf src/main.cpp src/analytics.cpp src/feature_store.cpp src/subscription_store.cpp src/collection_store.cpp src/ml_store.cpp src/model_runtime.cpp src/training_executor.cpp src/mtlf.cpp src/ml_consumer.cpp src/accuracy_monitor.cpp)
target_link_libraries(nwdaf PRIVATE sbi_core sbi_generated nf_config unofficial::libmariadb redis++::redis++_static onnxruntime::onnxruntime "$<LINK_LIBRARY:WHOLE_ARCHIVE,ONNX::onnx>")
target_compile_definitions(nwdaf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config" NWDAF_TRAINING_DIR="${CMAKE_SOURCE_DIR}/nfs/nwdaf/training")
