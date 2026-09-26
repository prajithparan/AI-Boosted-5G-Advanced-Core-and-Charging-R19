find_package(RdKafka CONFIG REQUIRED) # ADR-0355
find_package(redis++ CONFIG REQUIRED) # ADR-0369: the MTLF test clears NWDAF ML state in Valkey
find_package(libpqxx CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
# ADR-0283: the CDR retention test compiles CHF's own CdrWriter (see below), which talks to
# Doris over the MySQL protocol -- same package nfs/chf/CMakeLists.txt uses.
find_package(unofficial-libmariadb CONFIG REQUIRED)

add_executable(integration_tests
    test_hello_nf_registration.cpp
    test_nrf_gap_closure.cpp
    test_amf_namf_communication.cpp
    test_amf_namf_location_eventexposure.cpp
    test_amf_namf_aiot_mbs_mt.cpp
    test_smf_pdu_session.cpp
    test_smf_event_exposure_nidd.cpp
    test_udm_uecm_sdm.cpp
    test_udm_ueau.cpp
    test_udm_ueau_gap_closure_214.cpp
    test_udm_uecm_gap_closure_215.cpp
    test_udm_uecm_gap_closure_216.cpp
    test_udm_uecm_gap_closure_217.cpp
    test_udm_uecm_gap_closure_218.cpp
    test_udm_uecm_gap_closure_219.cpp
    test_udm_uecm_gap_closure_220.cpp
    test_udm_uecm_gap_closure_221.cpp
    test_udm_uecm_gap_closure_227.cpp
    test_udm_sdm_gap_closure_228.cpp
    test_udm_sdm_gap_closure_230.cpp
    test_udm_sdm_gap_closure_232.cpp
    test_udm_sdm_gap_closure_233.cpp
    test_udm_sdm_gap_closure_234.cpp
    test_udm_sdm_gap_closure_235.cpp
    test_udm_sdm_gap_closure_236.cpp
    test_udm_pp_gap_closure_237.cpp
    test_udm_mt_niddau_rsds_ssau_ueid.cpp
    test_upf_event_exposure_gueip.cpp
    test_udr_context_data.cpp
    test_udr_ondatachange_webhook.cpp
    test_sbi_core_concurrency.cpp
    test_http2_route_precedence.cpp
    test_udr_gap_closure_212.cpp
    test_udr_policy_data_gap_closure_213.cpp
    test_udr_exposure_data.cpp
    test_udr_aiot_data.cpp
    test_udr_oam_provisioning.cpp
    test_udm_ueau_from_udr.cpp
    test_product_catalog_lossless.cpp
    test_balance_lossless.cpp
    test_party_lossless.cpp
    # ADR-0382: the OAM provisioning validator + its store, compiled in (udr is an executable).
    ${CMAKE_SOURCE_DIR}/nfs/udr/src/oam_provisioning.cpp
    ${CMAKE_SOURCE_DIR}/nfs/udr/src/stores.cpp
    test_smf_handover_n2sminfo.cpp
    test_smf_n2sminfo_dispatch.cpp
    test_amf_ngap_handover.cpp
    test_ngap_codec_presence.cpp
    test_nsacf_slice_admission.cpp
    test_tps_spike_protection.cpp
    test_chaos_charging.cpp
    test_n28_sy_end_to_end.cpp
    test_udm_map_insert_subscriber_data.cpp
    test_chf_protocol_ceilings.cpp
    test_cap_scoped_charging.cpp
    test_cdr_event_bus.cpp
    test_nwdaf_phase_a.cpp
    test_mfaf.cpp
    test_dccf.cpp
    test_adrf.cpp
    test_nwdaf_phase_c.cpp
    test_proportional_debit.cpp
    test_charging_scope.cpp
    test_unit_pooling.cpp
    test_mcp_pii_governance.cpp
    test_protocol_attributes.cpp
    test_subscriber_lifecycle.cpp
    test_balance_shared_bucket.cpp
    test_cdr_billing_chain.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/billing_items.cpp
    test_udm_map_server.cpp
    test_nef_af_traffic_influence.cpp
    test_cdr_retention.cpp
    # ADR-0283: the retention test drives CHF's REAL CdrWriter against a real Doris, so its
    # translation units are compiled in rather than the behaviour being re-implemented in a
    # fake. CHF is an executable, so there is no library target to link instead.
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr_event_producer.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/proportional_debit.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/charging_scope.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/unit_pooling.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/protocol_attributes.cpp
    ${CMAKE_SOURCE_DIR}/tools/mcp-server/src/agent_scope.cpp
    ${CMAKE_SOURCE_DIR}/tools/mcp-server/src/pii_audit.cpp
    ${CMAKE_SOURCE_DIR}/tools/mcp-server/src/analytics.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr_asn1.cpp
    ngap_test_gnb.cpp
    ue_nas_driver.cpp
    test_ausf_ue_authentication.cpp
    test_ausf_upu_protection.cpp
    test_pcf_policy_control.cpp
    test_pcf_ue_policy_event_exposure.cpp
    test_pcf_am_mbs_policy_auth_control.cpp
    test_pcf_pdtq_bdt_policy_control.cpp
    test_nef_sms_ueid_dnaimapping_easdeployment.cpp
    test_nef_smcontext_authentication_ecsaddress.cpp
    test_nef_event_exposure.cpp
    test_nef_traffic_influence_inference_training.cpp
    test_n28_spending_limit.cpp
    test_product_catalog_postgres.cpp
    test_subscriber_management_postgres.cpp
    test_roaming_interconnect_postgres.cpp
)
# ngap_core/ngap_generated (ADR-0260): test_smf_n2sminfo_dispatch.cpp builds REAL PER-encoded
# NGAP transfers to drive SMF's N2SmInfoType branches. Asserting on bytes SMF returns is not
# enough for those -- the branches only run on a transfer that actually decodes, so the test
# has to be able to encode one.
target_include_directories(integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/nfs/chf/src
                                                    ${CMAKE_SOURCE_DIR}/nfs/udr/src
                                                    ${CMAKE_SOURCE_DIR}/tools/mcp-server/src)
target_link_libraries(integration_tests PRIVATE libpqxx::pqxx unofficial::libmariadb RdKafka::rdkafka++ ss7_core tcap_core map_core tbcd_core diameter_core cap_core sbi_core sbi_generated aka_crypto product_catalog_store subscriber_management_store roaming_interconnect_store balance_management_store provisioning_store ngap_core ngap_generated GTest::gtest GTest::gtest_main)
add_dependencies(integration_tests nrf amf smf udm udr ausf pcf chf hello-nf upf nef nsacf product-catalog balance-management nwdaf mfaf dccf adrf)
target_compile_definitions(integration_tests PRIVATE
    NRF_PATH="$<TARGET_FILE:nrf>"
    AMF_PATH="$<TARGET_FILE:amf>"
    SMF_PATH="$<TARGET_FILE:smf>"
    UDM_PATH="$<TARGET_FILE:udm>"
    UPF_PATH="$<TARGET_FILE:upf>"
    UDR_PATH="$<TARGET_FILE:udr>"
    AUSF_PATH="$<TARGET_FILE:ausf>"
    PCF_PATH="$<TARGET_FILE:pcf>"
    CHF_PATH="$<TARGET_FILE:chf>"
    # ADR-0353: the CAP scoping test needs the charging backends running, not just CHF --
    # a rating decision cannot be made without a catalog to rate against and a balance to
    # reserve from, which is exactly why the ceilings test could not assert one.
    PRODUCT_CATALOG_PATH="$<TARGET_FILE:product-catalog>"
    BALANCE_MANAGEMENT_PATH="$<TARGET_FILE:balance-management>"
    NWDAF_PATH="$<TARGET_FILE:nwdaf>"
    MFAF_PATH="$<TARGET_FILE:mfaf>"
    DCCF_PATH="$<TARGET_FILE:dccf>"
    ADRF_PATH="$<TARGET_FILE:adrf>"
    HELLO_NF_PATH="$<TARGET_FILE:hello-nf>"
    NEF_PATH="$<TARGET_FILE:nef>"
    NSACF_PATH="$<TARGET_FILE:nsacf>"
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs"
)

include(GoogleTest)
gtest_discover_tests(integration_tests)

# ADR-0369: the MTLF end-to-end test trains a model twice through the Python sidecar inside one
# test, which does not fit CI's 120 s per-test default; it lives in its own binary so ctest's
# TIMEOUT property (which wins over --timeout) can be set for it alone.
add_executable(nwdaf_mtlf_integration_tests test_nwdaf_mtlf.cpp)
target_link_libraries(nwdaf_mtlf_integration_tests PRIVATE redis++::redis++_static sbi_core sbi_generated GTest::gtest GTest::gtest_main)
add_dependencies(nwdaf_mtlf_integration_tests nrf adrf nwdaf)
target_compile_definitions(nwdaf_mtlf_integration_tests PRIVATE
    NRF_PATH="$<TARGET_FILE:nrf>"
    ADRF_PATH="$<TARGET_FILE:adrf>"
    NWDAF_PATH="$<TARGET_FILE:nwdaf>"
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs"
    NWDAF_TRAINING_DIR="${CMAKE_SOURCE_DIR}/nfs/nwdaf/training"
)
gtest_discover_tests(nwdaf_mtlf_integration_tests PROPERTIES TIMEOUT 400)

# ADR-0372: the LI_X2/LI_X3 delivery client over real OpenSSL mTLS. Its own binary -- it needs no
# NF processes (an in-process loopback TLS server stands in for the MDF), only li_core, OpenSSL,
# and the lab certs. Binds an ephemeral port, so it does not collide with CI's fixed test ports.
find_package(OpenSSL REQUIRED)
add_executable(li_x2x3_client_integration_tests test_li_x2x3_client.cpp)
target_link_libraries(li_x2x3_client_integration_tests PRIVATE
    li_core OpenSSL::SSL OpenSSL::Crypto GTest::gtest GTest::gtest_main)
target_compile_definitions(li_x2x3_client_integration_tests PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs")
gtest_discover_tests(li_x2x3_client_integration_tests)

# ADR-0375: the MDF2 receiving side, driven by the real client over real mTLS. Same shape -- its
# own binary, li_core + OpenSSL + the lab certs, an ephemeral port, no NF processes.
add_executable(li_x2x3_server_integration_tests test_li_x2x3_server.cpp)
target_link_libraries(li_x2x3_server_integration_tests PRIVATE
    li_core OpenSSL::SSL OpenSSL::Crypto GTest::gtest GTest::gtest_main)
target_compile_definitions(li_x2x3_server_integration_tests PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs")
gtest_discover_tests(li_x2x3_server_integration_tests)

# ADR-0376: the LI_HI2 Delivery Function against a loopback LEMF. Same shape as the two above.
add_executable(li_hi2_client_integration_tests test_li_hi2_client.cpp)
target_link_libraries(li_hi2_client_integration_tests PRIVATE
    li_core OpenSSL::SSL OpenSSL::Crypto GTest::gtest GTest::gtest_main)
target_compile_definitions(li_hi2_client_integration_tests PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs")
gtest_discover_tests(li_hi2_client_integration_tests)

# ADR-0377: the MDF2 process end to end -- a real li-mdf provisioned over real X1, fed over real
# X2, delivering over real HI2 to a loopback LEMF. Needs Valkey; skips without it.
add_executable(li_mdf_integration_tests test_li_mdf.cpp)
target_link_libraries(li_mdf_integration_tests PRIVATE
    li_core sbi_core OpenSSL::SSL OpenSSL::Crypto GTest::gtest GTest::gtest_main)
target_compile_definitions(li_mdf_integration_tests PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs"
    LI_MDF_PATH="$<TARGET_FILE:li-mdf>")
add_dependencies(li_mdf_integration_tests li-mdf)
gtest_discover_tests(li_mdf_integration_tests)

# ADR-0377: the AMF IRI-POI's own logic (X1 provisioning -> target store -> xIRI -> X2 emit),
# compiling nfs/amf/src/li_poi.cpp directly against a loopback MDF2 (li_core::X2X3Server) and a
# fake ADMF (sbi_core HTTP/2 client). No NF processes; the POI has no standalone binary. Uses a
# fixed LI_X1 port (the sbi server binds a fixed port) clear of the other tests'.
add_executable(li_amf_poi_integration_tests
    test_li_amf_poi.cpp ${CMAKE_SOURCE_DIR}/nfs/amf/src/li_poi.cpp)
target_include_directories(li_amf_poi_integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/nfs/amf/src)
target_link_libraries(li_amf_poi_integration_tests PRIVATE
    li_core sbi_core OpenSSL::SSL OpenSSL::Crypto GTest::gtest GTest::gtest_main)
target_compile_definitions(li_amf_poi_integration_tests PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs")
gtest_discover_tests(li_amf_poi_integration_tests)
