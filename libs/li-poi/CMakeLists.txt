# The generic half of an IRI-POI / triggered POI (TS 33.127 5.3.2, ADR-0463): the LI_X1 listener a
# POI is provisioned through, the per-warrant target store, the X1 keepalive monitor, the LI_X2
# delivery client and per-XID sequence numbers. The AMF, SMF (and later POIs) each keep only what is
# specific to them -- which identities they match, which xIRI records they build and when.
#
# Links sbi_core for the TLS 1.3 + mTLS HTTP/2 server only; a POI registers with no NRF
# (TS 33.127 keeps LI off the service plane).
add_library(li_poi STATIC src/poi_runtime.cpp)
target_include_directories(li_poi PUBLIC include)
target_link_libraries(li_poi PUBLIC li_core sbi_core)
target_compile_features(li_poi PUBLIC cxx_std_23)
