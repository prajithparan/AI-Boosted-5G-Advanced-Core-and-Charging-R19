#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <utility>
#include <vector>

// Private to nfs/udr -- not shared with any other NF, per CLAUDE.md's "no NF includes another NF's
// private headers" rule. Real PostgreSQL persistence (libpqxx), same "one shared connection, one
// mutex" discipline every other PostgreSQL-backed store in this project already uses (see
// bss/product-catalog/src/store.hpp) -- ADR-0068, gap-closure Tier 1a from the free5GC/open5gs
// source comparison (both real references treat UDR as a genuinely persistent repository; this
// project's own in-memory std::unordered_map version did not survive a restart, unlike either
// reference's own real store).
//
// Deliberately NOT the same class as nfs/udm/src/stores.hpp's AmfRegistrationStore/
// SmfRegistrationStore, even though the shape is similar: UDR's context-data group uses RFC 6902
// JSON Patch (nlohmann::json::patch(), matching nfs/nrf's own UpdateNFInstance), not UDM's RFC
// 7396 JSON Merge Patch (nlohmann::json::merge_patch()) -- see docs/DECISIONS.md ADR-0025 for why
// the two Nudr_DataRepository PATCH operations use a different patch standard than UDM's.

namespace udr {

// Backs the AMF 3GPP-access context group (QueryAmfContext3gpp, CreateAmfContext3gpp,
// AmfContext3gpp). Keyed by ueId (Supi) -- one AMF context per UE, per
// TS29505_Subscription_Data.yaml's `/subscription-data/{ueId}/context-data/amf-3gpp-access`
// resource (singular, not a collection). No delete operation exists for this resource in the
// spec (checked, not assumed) -- disclosed in nfs/udr/src/main.cpp's file header.
class AmfContextStore {
public:
    explicit AmfContextStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, nlohmann::json context);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Applies an RFC 6902 JSON Patch (already parsed) via nlohmann::json's built-in .patch().
    // Throws nlohmann::json::exception (invalid patch op, failed "test", ...) on a malformed
    // patch -- caller turns that into a 400 ProblemDetails, same as nfs/nrf's apply_patch.
    // Returns nullopt if ue_id doesn't exist.
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const nlohmann::json& patch_ops);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0093): backs the AMF non-3GPP-access
// context group (QueryAmfContextNon3gpp, CreateAmfContextNon3gpp -- real
// TS29505_Subscription_Data.yaml `/subscription-data/{ueId}/context-data/amf-non-3gpp-access`).
// Deliberately NOT the same class/table as AmfContextStore above -- a real, distinct resource per
// spec (schema `AmfNon3GppAccessRegistration`, not `Amf3GppAccessRegistration`), same "one UE can
// have both a 3GPP and a non-3GPP AMF context simultaneously" real architecture the two separate
// spec paths already imply. Real, confirmed (not assumed): no PATCH/DELETE operation exists for
// this resource in the spec, same as its 3GPP-access sibling.
class AmfNon3GppContextStore {
public:
    explicit AmfNon3GppContextStore(const std::string& conninfo);

    bool put(const std::string& ue_id, nlohmann::json context);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Backs the SMF registration context group (QuerySmfRegList, QuerySmfRegistration,
// CreateOrUpdateSmfRegistration, UpdateSmfContext, DeleteSmfRegistration). Keyed by
// (ueId, pduSessionId), same nested-key shape as nfs/udm's SmfRegistrationStore and for the same
// reason (QuerySmfRegList needs to list every registration for a given ueId).
class SmfRegistrationStore {
public:
    explicit SmfRegistrationStore(const std::string& conninfo);

    bool
    put(const std::string& ue_id, const std::string& pdu_session_id, nlohmann::json registration);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& pdu_session_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& pdu_session_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& pdu_session_id);
    std::vector<nlohmann::json> list_for_ue(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Backs the real Nudr_DataRepository `provisioned-data` group (am-data, smf-selection-
// subscription-data, sm-data, and -- ADR-0106, gap-closure task #106 -- lcs-bca-data) -- ADR-0069,
// gap-closure Tier 1b. Real, disclosed: this real resource group is GET-only per the spec (no
// create/update operation exists at all), so there is no put()/apply_patch() here -- only seed()
// (used once, at startup, same real-data-source reasoning as this NF's own schema.postgres.sql
// header) and the four real get*() accessors.
class ProvisionedDataStore {
public:
    explicit ProvisionedDataStore(const std::string& conninfo);

    // Real UPSERT -- idempotent, safe to call every startup even if rows already exist from a
    // prior run (same real persistence property Tier 1a's own stores already have).
    void seed(const std::string& ue_id,
              const std::string& serving_plmn_id,
              std::optional<nlohmann::json> am_data,
              std::optional<nlohmann::json> smf_sel_data,
              std::optional<nlohmann::json> sm_data,
              std::optional<nlohmann::json> lcs_bca_data,
              std::optional<nlohmann::json> sms_mng_data,
              std::optional<nlohmann::json> sms_data,
              std::optional<nlohmann::json> trace_data);

    std::optional<nlohmann::json> get_am_data(const std::string& ue_id,
                                              const std::string& serving_plmn_id);
    std::optional<nlohmann::json> get_smf_sel_data(const std::string& ue_id,
                                                   const std::string& serving_plmn_id);
    std::optional<nlohmann::json> get_sm_data(const std::string& ue_id,
                                              const std::string& serving_plmn_id);
    // ADR-0106, gap-closure task #106: real LCS Broadcast Assistance Subscription Data
    // (QueryLcsBcaData), same real GET-only path shape as the other three sub-resources above.
    std::optional<nlohmann::json> get_lcs_bca_data(const std::string& ue_id,
                                                   const std::string& serving_plmn_id);
    // ADR-0125, gap-closure task #106: real SMS Management Subscription Data (QuerySmsMngData),
    // same real GET-only path shape as the other sub-resources above.
    std::optional<nlohmann::json> get_sms_mng_data(const std::string& ue_id,
                                                   const std::string& serving_plmn_id);
    // ADR-0126, gap-closure task #106: real SMS Subscription Data (QuerySmsData), same real
    // GET-only path shape as the other sub-resources above.
    std::optional<nlohmann::json> get_sms_data(const std::string& ue_id,
                                               const std::string& serving_plmn_id);
    // ADR-0127, gap-closure task #106: real Trace Data (QueryTraceData), same real GET-only path
    // shape as the other sub-resources above. Real response schema is a `oneOf` (full `TraceData`
    // object or a bare `SharedDataId` string) -- returned as opaque JSON, same as every other
    // sub-resource in this store.
    std::optional<nlohmann::json> get_trace_data(const std::string& ue_id,
                                                 const std::string& serving_plmn_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// ADR-0072 (gap-closure: real N28 end-to-end): backs the real Nudr_DataRepository `policy-data`
// group's SM policy resource (TS29519_Policy_Data.yaml, `/policy-data/ues/{ueId}/sm-data`, real
// schema SmPolicyData -- genuinely distinct from ProvisionedDataStore's own `sm_data` column
// above, see schema.postgres.sql's own comment). Real RFC 7396 JSON Merge Patch
// (application/merge-patch+json, confirmed directly against the YAML -- same patch standard as
// UDM's own AmfRegistrationStore/SmfRegistrationStore, NOT AmfContextStore's RFC 6902 above).
// Deliberately upsert-capable (merge_patch creates a fresh document from `{}` if ueId doesn't
// exist yet) so this resource can be created from a future GUI even though the real spec defines
// no POST/create operation for it at all -- see schema.postgres.sql's own comment for why this is
// a disclosed, deliberate choice.
class SmPolicyDataStore {
public:
    explicit SmPolicyDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id);
    nlohmann::json merge_patch(const std::string& ue_id, const nlohmann::json& patch);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0083). Backs the real
// Authentication Data group's `authentication-subscription` document (QueryAuthSubsData,
// ModifyAuthenticationSubscription -- RFC 6902 JSON Patch, same standard AmfContextStore above
// uses, NOT SmPolicyDataStore's RFC 7396 merge-patch). See schema.postgres.sql's own comment for
// why this is a real, genuinely distinct table from UDM's own in-process
// AuthenticationSubscriptionStore. No create/delete operation exists in the real spec for this
// resource (checked, not assumed) -- `apply_patch` is upsert-capable (same disclosed,
// deliberate divergence SmPolicyDataStore's own header already established) so this store still
// has a real way to originate a document.
class AuthenticationSubscriptionDataStore {
public:
    explicit AuthenticationSubscriptionDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id);
    nlohmann::json apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Backs the real Authentication Data group's `authentication-status` document
// (CreateAuthenticationStatus/QueryAuthenticationStatus/DeleteAuthenticationStatus -- real PUT
// (replace, not patch) + GET + DELETE, confirmed per-operation from the YAML).
class AuthenticationStatusStore {
public:
    explicit AuthenticationStatusStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0212). Backs the real Individual
// Authentication Status (Document) resource (CreateIndividualAuthenticationStatus/
// QueryIndividualAuthenticationStatus/DeleteIndividualAuthenticationStatus -- real PUT (replace)
// + GET + DELETE per TS29505_Subscription_Data.yaml), a genuinely distinct resource from
// AuthenticationStatusStore above: keyed by (ueId, servingNetworkName), not ueId alone. Same real
// `AuthEvent` schema (TS29503_Nudm_UEAU.yaml) as the bare resource, same composite-key doc-store
// shape as ServiceSpecificAuthorizationInfoStore.
class IndividualAuthenticationStatusStore {
public:
    explicit IndividualAuthenticationStatusStore(const std::string& conninfo);

    void
    put(const std::string& ue_id, const std::string& serving_network_name, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id,
                                      const std::string& serving_network_name);
    bool remove(const std::string& ue_id, const std::string& serving_network_name);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Backs the real `policy-data` group's AM Policy resource (ReadAccessAndMobilityPolicyData,
// UpdateAccessAndMobilityPolicyData -- real GET + RFC 7396 merge-patch, the real UDR-side backing
// for PCF's own Npcf_AMPolicyControl). Same real upsert-on-PATCH shape as SmPolicyDataStore
// above.
// Gap-closure (ADR-0253, from ADR-0252's audit). Real Nudr_DataRepository `application-data`
// traffic-influence family (TS29519_Application_Data.yaml, $ref'd from TS29504_Nudr_DR.yaml).
// `list()` returns every stored document -- the real GET collection defines 8 query parameters
// (influence-Ids, dnns, snssais, internal-Group-Ids, internal-group-ids-Add,
// subscriber-categories, supis, supp-feat); which of those this build actually honours is
// documented at the route itself, not silently implied here.
// A CRUD store over an opaque JSON document keyed by one id column, parameterised by table.
// Introduced by ADR-0254 for the remaining real `application-data` resources (pfds,
// bdtPolicyData, iptvConfigData, serviceParamData, subs-to-notify) and reused by ADR-0255 for
// `exposure-data`'s access-and-mobility-data and subs-to-notify. Each of those is a genuinely
// distinct real resource with its OWN table -- distinctness is enforced by the schema, not by
// duplicated C++; this is deliberately NOT the "one table with a discriminator" shape the project
// has rejected before. Renamed from ApplicationDataDocStore by ADR-0255 because it is no longer
// specific to the application-data family; behaviour is unchanged.
//
// `table_`/`id_column_` are interpolated into SQL. They are fixed literals chosen at construction
// in main.cpp and are NEVER request-derived; values are always bound as parameters.
class KeyedJsonDocStore {
public:
    KeyedJsonDocStore(const std::string& conninfo, std::string table, std::string id_column);

    std::vector<nlohmann::json> list();
    std::optional<nlohmann::json> get(const std::string& id);
    // true when the row did not previously exist (the real 201-vs-200/204 distinction).
    bool put(const std::string& id, const nlohmann::json& data);
    // RFC 7396 merge patch. std::nullopt when the resource does not exist -- the real spec
    // documents 404 for PATCH here, so this deliberately does NOT upsert.
    std::optional<nlohmann::json> merge_patch(const std::string& id, const nlohmann::json& patch);
    bool remove(const std::string& id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
    std::string table_;
    std::string id_column_;
};

// ADR-0255: `/exposure-data/{ueId}/session-management-data/{pduSessionId}`
// (TS29519_Exposure_Data.yaml). Its own class rather than a KeyedJsonDocStore because the
// resource is keyed by TWO path parameters, not one -- a UE has one document per PDU session.
// The real spec gives this resource PUT/GET/DELETE and NO PATCH (unlike its
// access-and-mobility-data sibling), so no merge_patch() is offered here.
class ExposureSessionManagementDataStore {
public:
    explicit ExposureSessionManagementDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& pdu_session_id);
    // true when the row did not previously exist (the real 201-vs-200 distinction).
    bool
    put(const std::string& ue_id, const std::string& pdu_session_id, const nlohmann::json& data);
    bool remove(const std::string& ue_id, const std::string& pdu_session_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// ADR-0256: AIoT device profile data (`/aiot-data/aiot-device-profile-data`,
// TS29506_Aiot_Data.yaml; schemas from TS29369_Nadm_DM.yaml). Its own class rather than a
// KeyedJsonDocStore because the bundled GET filters by an explicit list of ids, and PATCH here is
// RFC 6902 JSON Patch, not the RFC 7396 merge-patch KeyedJsonDocStore offers.
//
// Real, disclosed: UDR's own spec gives this resource **only GET and PATCH** -- there is no
// create/replace/delete operation anywhere in TS29506_Aiot_Data.yaml -- so there is no live
// provisioning path and rows are seeded at startup. Same shape as the `provisioned-data` group
// (ADR-0069) and RoutingIdStore/NfGroupIdStore, not an omission here.
class AiotDeviceProfileDataStore {
public:
    explicit AiotDeviceProfileDataStore(const std::string& conninfo);

    void seed(const std::string& aiot_dev_perm_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& aiot_dev_perm_id);
    // Bundled read: returns only the requested ids, in the order asked for, skipping ids that
    // have no row. The real GET makes `requester-aiot-devices-id` a REQUIRED query parameter, so
    // there is deliberately no "list everything" entry point.
    std::vector<nlohmann::json> get_many(const std::vector<std::string>& aiot_dev_perm_ids);
    // Real RFC 6902 JSON Patch (already parsed) via nlohmann::json's built-in .patch(). Throws
    // nlohmann::json::exception on an invalid patch -- the caller catches. std::nullopt when the
    // row does not exist (the real spec documents 404 for this operation).
    std::optional<nlohmann::json> patch(const std::string& aiot_dev_perm_id,
                                        const nlohmann::json& patch_ops);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// ADR-0256: `/aiot-data/af-authorization-data`. A genuinely keyless singleton document
// (`AfAuthorizationData` is one object whose `afAuthData` is a map keyed by AF id), backed by a
// fixed single-row table -- the same shape as FiveGVnGroupPpProfileDataStore. GET is the only
// operation the spec defines, so seed() + get() only.
class AiotAfAuthorizationDataStore {
public:
    explicit AiotAfAuthorizationDataStore(const std::string& conninfo);

    void seed(nlohmann::json data);
    std::optional<nlohmann::json> get();

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// ADR-0256: `/data-restoration-events` (TS29504_Nudr_DR.yaml). The real request body schema is
// literally `{}` -- the YAML defines no fields at all -- so the subscription is persisted opaquely
// under a server-assigned id rather than validated against a shape that does not exist. There is
// no individual-subscription resource path in the spec, so there is deliberately no get/remove
// here: nothing in the API can address a stored row.
class DataRestorationSubscriptionStore {
public:
    explicit DataRestorationSubscriptionStore(const std::string& conninfo);

    void add(const std::string& subscription_id, const nlohmann::json& data);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

class TrafficInfluenceDataStore {
public:
    explicit TrafficInfluenceDataStore(const std::string& conninfo);

    std::vector<nlohmann::json> list();
    // ADR-0302: the same rows paired with their own influenceId, which the stored documents do NOT
    // carry -- `influenceId` is the resource key from the URL path, not a field of the real
    // TrafficInfluData schema. The collection GET's `influence-Ids` filter needs the key, and
    // matching it against a document field it can never contain is why that filter silently
    // returned nothing for every request (found by ADR-0302's NEF broker test).
    std::vector<std::pair<std::string, nlohmann::json>> list_with_ids();
    std::optional<nlohmann::json> get(const std::string& influence_id);
    // Returns true when the row did not exist (real 201 vs 200 distinction the spec draws).
    bool put(const std::string& influence_id, const nlohmann::json& data);
    // RFC 7396 JSON Merge Patch -- confirmed from the YAML's own
    // `application/merge-patch+json` request content type, not assumed from other UDR resources.
    std::optional<nlohmann::json> merge_patch(const std::string& influence_id,
                                              const nlohmann::json& patch);
    bool remove(const std::string& influence_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// The change-subscription resource over the above. A genuinely separate real resource with its
// own schema (TrafficInfluSub), not a view of the same table.
class TrafficInfluenceSubStore {
public:
    explicit TrafficInfluenceSubStore(const std::string& conninfo);

    std::vector<nlohmann::json> list();
    std::optional<nlohmann::json> get(const std::string& subscription_id);
    bool put(const std::string& subscription_id, const nlohmann::json& data);
    bool remove(const std::string& subscription_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

class AmPolicyDataStore {
public:
    explicit AmPolicyDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id);
    nlohmann::json merge_patch(const std::string& ue_id, const nlohmann::json& patch);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0097). Backs the real SMSF
// 3GPP-access context-data resource (CreateSmsfContext3gpp/QuerySmsfContext3gpp/
// DeleteSmsfContext3gpp -- real GET+PUT+DELETE, same shape as AuthenticationStatusStore above).
// Deliberately NOT the same class as SmsfNon3GppContextStore below, even though both real spec
// resources share the identical schema (`SmsfRegistration`) -- same "real, distinct resource, not
// a rename" precedent AmfContextStore/AmfNon3GppContextStore already established.
class SmsfContext3gppStore {
public:
    explicit SmsfContext3gppStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Backs the real SMSF non-3GPP-access context-data resource (CreateSmsfContextNon3gpp/
// QuerySmsfContextNon3gpp/DeleteSmsfContextNon3gpp) -- see SmsfContext3gppStore's own comment for
// why this is a separate class/table.
class SmsfNon3GppContextStore {
public:
    explicit SmsfNon3GppContextStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0098). Backs the real IP-SM-GW
// Registration context-data resource (CreateIpSmGwContext/QueryIpSmGwContext/
// ModifyIpSmGwContext/DeleteIpSmGwContext -- real PUT+GET+PATCH+DELETE, the richest operation set
// of any context-data resource this project has closed so far). Real RFC 6902 JSON Patch (same
// standard AmfContextStore's own apply_patch already uses), not RFC 7396 merge-patch.
class IpSmGwContextStore {
public:
    explicit IpSmGwContextStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json context);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Throws nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as AmfContextStore's own apply_patch. Returns nullopt if ue_id doesn't
    // exist.
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0099). Backs the real Message
// Waiting Data (Document) resource (CreateMessageWaitingData/QueryMessageWaitingData/
// ModifyMessageWaitingData/DeleteMessageWaitingData -- real PUT+GET+PATCH+DELETE). Unlike
// IpSmGwContextStore's own always-204 put(), MWD's real PUT genuinely distinguishes 201-Created
// from 204-updated per the YAML -- same real "xmax = 0" UPSERT idiom AmfContextStore's own put()
// already established, reused here rather than IpSmGwContextStore's simpler always-update one.
class MessageWaitingDataStore {
public:
    explicit MessageWaitingDataStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Throws nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as IpSmGwContextStore's own apply_patch. Returns nullopt if ue_id
    // doesn't exist.
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0100). Backs the real Roaming
// Information (Document) resource (UpdateRoamingInformation/QueryRoamingInformation -- real
// GET+PUT, same shape as AmfNon3GppContextStore above, including the real distinct 201-vs-204 PUT
// response codes). No PATCH/DELETE exists for this resource in the spec (checked, not assumed).
class RoamingInformationStore {
public:
    explicit RoamingInformationStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0101). Backs the real PEI
// Information (Document) resource (CreateOrUpdatePeiInformation/QueryPeiInformation -- real
// GET+PUT, same shape as RoamingInformationStore above, including the real distinct 201-vs-204
// PUT response codes). No PATCH/DELETE exists for this resource in the spec (checked, not
// assumed).
class PeiInfoStore {
public:
    explicit PeiInfoStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0102). Backs the real Enhanced
// Coverage Restriction Data resource (QueryCoverageRestrictionData -- real GET-only, no
// create/update operation exists in the spec at all, same real "provisioned out-of-band, seeded
// at startup" shape as ProvisionedDataStore above).
class CoverageRestrictionDataStore {
public:
    explicit CoverageRestrictionDataStore(const std::string& conninfo);

    // Real UPSERT -- idempotent, safe to call every startup even if rows already exist from a
    // prior run (same real persistence property ProvisionedDataStore's own seed() has).
    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0103). Backs the real LCS Privacy
// Subscription Data resource (QueryLcsPrivacyData -- real GET-only, no create/update operation
// exists in the spec at all, same shape as CoverageRestrictionDataStore above).
class LcsPrivacyDataStore {
public:
    explicit LcsPrivacyDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0104). Backs the real LCS
// Subscription Data resource (QueryLcsSubscriptionData -- real GET-only, no create/update
// operation exists in the spec at all, same shape as LcsPrivacyDataStore above).
class LcsSubscriptionDataStore {
public:
    explicit LcsSubscriptionDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0105). Backs the real LCS Mobile
// Originated Subscription Data resource (QueryLcsMoData -- real GET-only, no create/update
// operation exists in the spec at all, same shape as LcsSubscriptionDataStore above).
class LcsMoDataStore {
public:
    explicit LcsMoDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0107). Backs the real Parameter
// Provision (Document) resource (GetppData/ModifyPpData -- real GET+PATCH, RFC 6902, no
// PUT/DELETE exists for this resource in the spec). No POST/create operation exists either, so
// apply_patch() is upsert-capable (missing ueId = start from an empty document) -- same disclosed,
// deliberate precedent already established for AuthenticationSubscriptionDataStore/
// SmPolicyDataStore.
class PpDataStore {
public:
    explicit PpDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Throws nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as AuthenticationSubscriptionDataStore's own apply_patch.
    nlohmann::json apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0108). Backs the real Parameter
// Provision profile Data (Document) resource (QueryPPData -- real GET-only, no create/update
// operation exists in the spec at all, same shape as the other GET-only UDR resources).
class PpProfileDataStore {
public:
    explicit PpProfileDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0109). Backs the real Provisioned
// Parameter Data Entry resource (Create/Get/Delete PP Data Entry -- real PUT+GET+DELETE) and its
// real sibling collection resource (Get Multiple PP Data Entries). Composite key
// (ue_id, af_instance_id), same real shape as SmfRegistrationStore's own (ue_id, pdu_session_id).
class PpDataEntryStore {
public:
    explicit PpDataEntryStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, const std::string& af_instance_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& af_instance_id);
    bool remove(const std::string& ue_id, const std::string& af_instance_id);
    std::vector<nlohmann::json> list_for_ue(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0110). Backs the real individual
// Shared Data resource (GetIndividualSharedData -- real GET-only, no create/update operation
// exists in the spec at all). Genuinely NOT per-UE -- keyed by shared_data_id alone.
class SharedDataStore {
public:
    explicit SharedDataStore(const std::string& conninfo);

    void seed(const std::string& shared_data_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& shared_data_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0111; PUT/DELETE added ADR-0197,
// correcting ADR-0111's own real documentation error -- it claimed "no PUT/DELETE exists for this
// resource," which a direct re-read of TS29505_Subscription_Data.yaml disproved:
// `CreateOperSpecData` (PUT) and `DeleteOperSpecData` (DELETE) are both real, declared operations
// alongside QueryOperSpecData/ModifyOperSpecData). apply_patch() stays upsert-capable (same
// disclosed, deliberate precedent already established for PpDataStore) -- PUT/DELETE are additional
// real operations, not a replacement for that behavior.
class OperatorSpecificDataStore {
public:
    explicit OperatorSpecificDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Throws nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as PpDataStore's own apply_patch.
    nlohmann::json apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops);
    // Real PUT (CreateOperSpecData): create-or-replace. Returns true if this was a genuinely new
    // resource (the real YAML's own `201`+`Location` case), false if it replaced an existing one
    // (the real YAML's own `204` case) -- a real INSERT-vs-UPDATE distinction, not invented.
    bool put(const std::string& ue_id, nlohmann::json data);
    // Real DELETE (DeleteOperSpecData). Returns false if no such resource existed (real `404`).
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0112). Backs the real Event
// Exposure Data (Document) resource (QueryEEData -- real GET-only, no create/update operation
// exists in the spec at all, same shape as the other GET-only UDR resources).
class EeProfileDataStore {
public:
    explicit EeProfileDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0113). Backs the real `policy-data`
// group's UE Policy Set resource (ReadUEPolicySet/CreateOrReplaceUEPolicySet/UpdateUEPolicySet --
// real GET+PUT+PATCH, RFC 7396 merge-patch, no DELETE exists for this resource in the spec). Real
// distinct 201-vs-204 PUT response codes, same real `xmax = 0` UPSERT idiom already established.
class UePolicySetStore {
public:
    explicit UePolicySetStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Real RFC 7396 JSON Merge Patch -- upsert-capable, matching AmPolicyDataStore's own
    // merge_patch() shape.
    nlohmann::json merge_patch(const std::string& ue_id, const nlohmann::json& patch);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0114). Backs the real
// `policy-data` group's Operator-Specific Data resource (ReadOperatorSpecificData/
// UpdateOperatorSpecificData -- real GET+PATCH, RFC 6902; PUT/DELETE added ADR-0197, correcting
// ADR-0114's own real documentation error -- it claimed "no PUT/DELETE exists for this resource,"
// which a direct re-read of TS29519_Policy_Data.yaml disproved: `ReplaceOperatorSpecificData`
// (PUT) and `DeleteOperatorSpecificData` (DELETE) are both real, declared operations). Real,
// genuinely distinct resource from OperatorSpecificDataStore above (separate real path/operationId
// pair, same schema reused via a real cross-file $ref). apply_patch() stays upsert-capable.
class PolicyOperatorSpecificDataStore {
public:
    explicit PolicyOperatorSpecificDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Throws nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as OperatorSpecificDataStore's own apply_patch.
    nlohmann::json apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops);
    // Real PUT (ReplaceOperatorSpecificData): create-or-replace, same real INSERT-vs-UPDATE
    // distinction as OperatorSpecificDataStore::put's own comment.
    bool put(const std::string& ue_id, nlohmann::json data);
    // Real DELETE (DeleteOperatorSpecificData). Returns false if no such resource existed.
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0115). Backs the real
// `policy-data` group's Sponsor Connectivity Data resource (ReadSponsorConnectivityData -- real
// GET-only, no create/update operation exists in the spec at all). Genuinely NOT per-UE -- keyed
// by sponsor_id alone.
class SponsorConnectivityDataStore {
public:
    explicit SponsorConnectivityDataStore(const std::string& conninfo);

    void seed(const std::string& sponsor_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& sponsor_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0116). Backs the real
// `policy-data` group's individual BDT (Background Data Transfer) Data resource
// (ReadIndividualBdtData/CreateIndividualBdtData/UpdateIndividualBdtData/
// DeleteIndividualBdtData -- real GET+PUT+PATCH+DELETE). Real, disclosed: put() is internally
// upsert-capable (idempotent-safe for retries) but the real spec's own PUT documents ONLY `201`
// as a success response (operationId literally "Create...", no update-via-PUT status
// documented) -- the caller always responds 201, not 204, matching the real spec literally.
class BdtDataStore {
public:
    explicit BdtDataStore(const std::string& conninfo);

    void put(const std::string& bdt_ref_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& bdt_ref_id);
    // Real RFC 7396 JSON Merge Patch.
    std::optional<nlohmann::json> merge_patch(const std::string& bdt_ref_id,
                                              const nlohmann::json& patch);
    bool remove(const std::string& bdt_ref_id);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0213). Backs the real bare
    // `ReadBdtData` collection GET -- every stored `BdtData`, the real optional `bdt-ref-ids`
    // filter applied by the caller (same "store returns all, route filters" precedent as
    // `PdtqDataStore::list()`/the route's own `wanted()` idiom elsewhere in this file).
    std::vector<nlohmann::json> list_all();

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0117). Backs the real PLMN UE
// Policy Set resource (/policy-data/plmns/{plmnId}/ue-policy-set, ReadPlmnUePolicySet -- real
// GET-only, no create/update operation exists for this resource at all, same real "provisioned
// out-of-band, seeded at startup" shape as CoverageRestrictionDataStore above). Reuses the real
// UePolicySet schema (same type as udr_ue_policy_set's own per-UE resource) but keyed by plmn_id,
// a genuinely distinct resource per TS29519_Policy_Data.yaml -- not a UE-scoped alias.
class PlmnUePolicySetStore {
public:
    explicit PlmnUePolicySetStore(const std::string& conninfo);

    void seed(const std::string& plmn_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& plmn_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0118). Backs the real Slice-specific
// Policy Control Data resource (/policy-data/slice-control-data/{snssai}, real GET+PATCH-only,
// no PUT/POST create operation exists at all -- confirmed by direct YAML read). Same disclosed,
// deliberate "no create operation exists, so merge_patch is upsert-capable" precedent already
// established for AmPolicyDataStore/SmPolicyDataStore, byte-for-byte matching AmPolicyDataStore's
// own class shape. Keyed by snssai (a plain string per this project's own established Snssai
// string-key convention).
class SlicePolicyDataStore {
public:
    explicit SlicePolicyDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& snssai);
    nlohmann::json merge_patch(const std::string& snssai, const nlohmann::json& patch);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0119). Backs the real group-specific
// Policy Control Data resource (/policy-data/group-control-data/{intGroupId}, real
// GET+PATCH-only, no PUT/POST create operation exists at all -- confirmed by direct YAML read).
// Same disclosed, deliberate "no create operation exists, so merge_patch is upsert-capable"
// precedent already established for AmPolicyDataStore/SlicePolicyDataStore. Keyed by intGroupId
// (real GroupId schema, TS29571_CommonData.yaml -- plain string, real pattern cited from
// TS 23.003 clause 19.9, no encoding ambiguity unlike SlicePolicyDataStore's own snssai key).
class GroupPolicyDataStore {
public:
    explicit GroupPolicyDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& int_group_id);
    nlohmann::json merge_patch(const std::string& int_group_id, const nlohmann::json& patch);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0120). Backs the real GetRoutingIDs
// resource (/routing-ids, TS29504_Nudr_GroupIDmap.yaml -- a genuinely DIFFERENT real Nudr API from
// every other store in this file, `Nudr_GroupIDmap` not `Nudr_DataRepository`: distinct real
// server base path (`/nudr-group-id-map/v1`, not `/nudr-dr/v2`) and distinct real OAuth2 scope
// (`nudr-group-id-map`, not `nudr-dr`). Real GET-only, no create/update operation exists for this
// resource at all -- same "provisioned out-of-band, seeded at startup" shape as every other
// GET-only store in this file, composite-keyed by (nf_type, nf_group_id) per the real spec's own
// two required query parameters, matching PpDataEntryStore's own composite-key precedent.
class RoutingIdStore {
public:
    explicit RoutingIdStore(const std::string& conninfo);

    void seed(const std::string& nf_type, const std::string& nf_group_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& nf_type, const std::string& nf_group_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0121). Backs the real NIDD
// Authorization Info context-data resource (CreateNIDDAuthorizationInfo/GetNiddAuthorizationInfo/
// ModifyNiddAuthorizationInfo/RemoveNiddAuthorizationInfo -- real PUT+GET+PATCH+DELETE). Real,
// disclosed correction: this project's own header comments previously lumped `nidd-authorizations`
// in with `ee-subscriptions`/`sdm-subscriptions` as a deferred "deeply nested sub-subscription"
// resource without individually checking the real YAML -- it is genuinely a flat per-UE document,
// same shape as AmfContextStore's own real distinct-201-vs-204 PUT + RFC 6902 JSON Patch, plus a
// real DELETE (which AmfContextStore's own resource doesn't have). Keyed by ueId (Supi).
class NiddAuthorizationInfoStore {
public:
    explicit NiddAuthorizationInfoStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Real RFC 6902 JSON Patch (already parsed) via nlohmann::json's built-in .patch(). Throws
    // nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as AmfContextStore's own apply_patch. Returns nullopt if ue_id doesn't
    // exist.
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0122). Backs the real Query/Modify
// Identity Data by SUPI or GPSI resource (GetIdentityData/ModifyIdentityData -- real GET+PATCH,
// no PUT/POST create operation exists at all -- confirmed by direct YAML read). Same disclosed,
// deliberate "no create operation exists, so apply_patch is upsert-capable" precedent already
// established for PpDataStore/OperatorSpecificDataStore (real RFC 6902 JSON Patch, not RFC 7396
// merge-patch, unlike slice-control-data/group-control-data's own PATCH standard). Real, disclosed
// simplification: the real spec's optional `app-port-id` query param (GET) and conditional-request
// headers (If-None-Match/If-Modified-Since, Cache-Control/ETag/Last-Modified on the response) are
// not implemented -- same "no conditional-GET semantics anywhere in this project yet" gap as every
// other GET route.
class IdentityDataStore {
public:
    explicit IdentityDataStore(const std::string& conninfo);

    std::optional<nlohmann::json> get(const std::string& ue_id);
    // Throws nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as PpDataStore's own apply_patch.
    nlohmann::json apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0123). Backs the real Query ODB
// Data by SUPI or GPSI resource (GetOdbData -- real GET-only, no create/update operation exists
// in the spec at all, same real "provisioned out-of-band, seeded at startup" shape as
// CoverageRestrictionDataStore above).
class OdbDataStore {
public:
    explicit OdbDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0128). Backs the real V2X
// Subscription Data resource (QueryV2xData -- real GET-only, no create/update operation exists in
// the spec at all, same real "provisioned out-of-band, seeded at startup" shape as
// CoverageRestrictionDataStore above). Keyed by `ueId` alone -- genuinely NOT part of the
// `provisioned-data` group's own `(ueId, servingPlmnId)` composite key shape.
class V2xDataStore {
public:
    explicit V2xDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0129). Backs the real ProSe Service
// Subscription Data resource -- real GET-only, no create/update operation exists in the spec at
// all, same real "provisioned out-of-band, seeded at startup" shape as V2xDataStore above. Real,
// disclosed: the spec's own operationId for this path is `QueryPorseData` (a real, literal typo
// in TS29505_Subscription_Data.yaml -- "Porse" not "Prose"), cited as-is, not corrected, since
// this project never invents or "fixes" spec text. Keyed by `ueId` alone.
class ProseDataStore {
public:
    explicit ProseDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0130). Backs the real User Consent
// Subscription Data resource (QueryUserConsentData -- real GET-only, no create/update operation
// exists in the spec at all, same real "provisioned out-of-band, seeded at startup" shape as
// ProseDataStore above). Real schema `UcSubscriptionData` (TS29503_Nudm_SDM.yaml) is a single
// optional `userConsentPerPurposeList` map, no `required` fields at all. Keyed by `ueId` alone.
class UcDataStore {
public:
    explicit UcDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0131). Backs the real Time
// Synchronization Subscription Data resource (QueryTimeSyncSubscriptionData -- real GET-only, no
// create/update operation exists in the spec at all, same real "provisioned out-of-band, seeded
// at startup" shape as UcDataStore above). Real schema `TimeSyncSubscriptionData`
// (TS29503_Nudm_SDM.yaml) requires `afReqAuthorizations` + `serviceIds`, unlike the last several
// GET-only resources closed which had every field optional. Keyed by `ueId` alone.
class TimeSyncDataStore {
public:
    explicit TimeSyncDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0133). Backs the real UE's Location
// Information (Document) resource (QueryUeLocation -- real GET-only, no create/update operation
// exists in the spec at all, same real "provisioned out-of-band, seeded at startup" shape as
// TimeSyncDataStore above). Real schema `LocationInfo` (TS29503_Nudm_UECM.yaml) requires a
// non-empty `registrationLocationInfoList`. Keyed by `ueId` alone.
class LocationDataStore {
public:
    explicit LocationDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0134). Backs the real A2X
// Subscription Data resource (QueryA2xData -- real GET-only, no create/update operation exists in
// the spec at all, same real "provisioned out-of-band, seeded at startup" shape as
// LocationDataStore above). Real schema `A2xSubscriptionData` (TS29503_Nudm_SDM.yaml) has every
// field optional, same shape as V2xDataStore/ProseDataStore. Keyed by `ueId` alone.
class A2xDataStore {
public:
    explicit A2xDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0135). Backs the real Ranging and
// Sidelink Positioning Privacy Subscription Data resource (QueryRangingSlPrivacyData -- real
// GET-only, no create/update operation exists in the spec at all, same real "provisioned
// out-of-band, seeded at startup" shape as A2xDataStore above). Real, disclosed: the spec's own
// optional `fields` query parameter for field-selection filtering is not honored -- the full
// stored document is always returned. Keyed by `ueId` alone.
class RangingSlPrivacyDataStore {
public:
    explicit RangingSlPrivacyDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0136). Backs the real Ranging and
// Sidelink Positioning Service Subscription Data resource (QueryRangingSlPosData -- real
// GET-only, no create/update operation exists in the spec at all, same real "provisioned
// out-of-band, seeded at startup" shape as RangingSlPrivacyDataStore above). Real schema
// `RangingSlPosSubscriptionData` (TS29503_Nudm_SDM.yaml) has every top-level field optional.
// Keyed by `ueId` alone.
class RangingSlPosDataStore {
public:
    explicit RangingSlPosDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0137). Backs the real 5MBS
// Subscription Data (Document) resource (Query5mbsData -- real GET-only, no create/update
// operation exists in the spec at all, same real "provisioned out-of-band, seeded at startup"
// shape as RangingSlPosDataStore above). Real schema `MbsSubscriptionData`
// (TS29503_Nudm_SDM.yaml) has every field optional. Keyed by `ueId` alone.
class MbsDataStore {
public:
    explicit MbsDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0139). Backs the real Service
// Specific Authorization Info (Document) context-data resource
// (CreateServiceSpecificAuthorizationInfo/GetServiceSpecificAuthorizationInfo/
// ModifyServiceSpecificAuthorizationInfo/RemoveServiceSpecificAuthorizationInfo -- real
// PUT+GET+PATCH+DELETE, same shape as NiddAuthorizationInfoStore). Composite key (ue_id,
// service_type) matches PpDataEntryStore's own precedent -- serviceType is a real plain-string
// enum, no encoding ambiguity.
class ServiceSpecificAuthorizationInfoStore {
public:
    explicit ServiceSpecificAuthorizationInfoStore(const std::string& conninfo);

    // Returns true if this was a new entry (for 201-vs-204 response selection).
    bool put(const std::string& ue_id, const std::string& service_type, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& service_type);
    // Real RFC 6902 JSON Patch (already parsed) via nlohmann::json's built-in .patch(). Throws
    // nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as NiddAuthorizationInfoStore's own apply_patch. Returns nullopt if
    // the (ue_id, service_type) pair doesn't exist.
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& service_type,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& service_type);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0140). Backs the real Group
// Identifiers mapping resource (GetGroupIdentifiers -- real GET-only, genuinely NOT per-UE, no
// path parameters at all). Real, disclosed: `extGroupId` and `intGroupId` are alternate lookup
// keys for the same seeded record; `ueIdInd` (whether to include ueIdList) is not honored --
// ueIdList is always included.
class GroupIdentifiersStore {
public:
    explicit GroupIdentifiersStore(const std::string& conninfo);

    void
    seed(const std::string& ext_group_id, const std::string& int_group_id, nlohmann::json data);
    std::optional<nlohmann::json> get_by_ext_group_id(const std::string& ext_group_id);
    std::optional<nlohmann::json> get_by_int_group_id(const std::string& int_group_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0141). Backs the real NSSAI update
// ack (Document) resource (CreateOrUpdateNssaiAck/QueryNssaiAck -- real PUT+GET, no PATCH/DELETE
// operation exists in the spec at all). Real, disclosed: unlike this project's other PUT
// resources, the spec documents only a single `204` response for this PUT (no `201`) -- no
// create-vs-update distinction exists, so put() returns void, not a bool. Keyed by ue_id.
class NssaiAckDataStore {
public:
    explicit NssaiAckDataStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0142). Backs the real CAG update
// ack (Document) resource (CreateCagUpdateAck/QueryCagAck -- real PUT+GET, no PATCH/DELETE
// operation exists in the spec at all, identical shape to NssaiAckDataStore above). Same real
// disclosed "204-only PUT, no create-vs-update distinction" shape. Keyed by ue_id.
class CagAckDataStore {
public:
    explicit CagAckDataStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0143). Backs the real Authentication
// SoR (Document) resource (CreateAuthenticationSoR/QueryAuthSoR/UpdateAuthenticationSoR -- real
// PUT+GET+PATCH per TS29505_Subscription_Data.yaml). Same real "204-only PUT" shape as
// NssaiAckDataStore/CagAckDataStore (the spec documents only a single `204` PUT response, no
// `201`), but genuinely richer than either: a real RFC 6902 application/json-patch+json PATCH
// exists too. apply_patch is NOT upsert-capable (returns nullopt if ue_id doesn't exist) -- unlike
// pp-data/operator-specific-data, this resource already has a real PUT create path, same
// precedent as NiddAuthorizationInfoStore. Keyed by ue_id.
class SorDataStore {
public:
    explicit SorDataStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const nlohmann::json& patch_ops);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0143). Backs the real Authentication
// UPU (Document) resource (CreateAuthenticationUPU/QueryAuthUPU -- real PUT+GET only, no
// PATCH/DELETE operation exists in the spec at all -- genuinely narrower than SorDataStore above
// despite sharing the same UeUpdateStatus-based schema shape). Same real "204-only PUT, no
// create-vs-update distinction" shape as NssaiAckDataStore/CagAckDataStore. Keyed by ue_id.
class UpuDataStore {
public:
    explicit UpuDataStore(const std::string& conninfo);

    void put(const std::string& ue_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0144). Backs the real
// `group-data`/`5g-vn-groups/{externalGroupId}` individual 5G VN Group Configuration resource
// (Create5GVnGroup/Get5GVnGroupConfiguration/Modify5GVnGroup/Delete5GVnGroup -- real
// GET+PUT+PATCH+DELETE per TS29505_Subscription_Data.yaml, schema `5GVnGroupConfiguration`
// generated as `sbi_gen::N5GVnGroupConfiguration`). Real, disclosed: put() is internally
// upsert-capable (idempotent-safe for retries) but the real spec's own PUT documents ONLY `201`
// as a success response (operationId literally "Create...", no update-via-PUT status documented)
// -- same real precedent as `BdtDataStore`, the caller always responds 201, not 204. PATCH is
// real RFC 6902 `application/json-patch+json` (confirmed by direct YAML read, NOT the RFC 7396
// merge-patch `BdtDataStore` itself uses) and is NOT upsert-capable -- PUT is the real create
// path, same precedent as `SorDataStore`/`NiddAuthorizationInfoStore`. Keyed by `externalGroupId`
// (real schema `ExtGroupId`, a plain string). First real `group-data` sub-resource closed since
// `group-identifiers` (ADR-0140).
class FiveGVnGroupStore {
public:
    explicit FiveGVnGroupStore(const std::string& conninfo);

    void put(const std::string& ext_group_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ext_group_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ext_group_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ext_group_id);
    // Real Query5GVnGroup (ADR-0167) -- every persisted row, keyed by ext_group_id, for the bare
    // collection GET to compose into its own real map response.
    std::vector<std::pair<std::string, nlohmann::json>> list_all();

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0145). Backs the real
// `group-data`/`mbs-group-membership/{externalGroupId}` individual 5G MBS Group Membership
// resource (Create5GmbsGroup/GetMulticastMbsGroupMemb/Modify5GmbsGroup/Delete5GmbsGroup -- real
// GET+PUT+PATCH+DELETE per TS29505_Subscription_Data.yaml, schema `MulticastMbsGroupMemb`).
// Structurally an exact twin of `FiveGVnGroupStore` above: `put()` internally upsert-capable but
// the real PUT documents ONLY `201`; PATCH is real RFC 6902, NOT upsert-capable. Keyed by
// `externalGroupId` (real schema `ExtGroupId`, a plain string). Second real `group-data`
// sub-resource closed, after `5g-vn-groups/{externalGroupId}` (ADR-0144).
class MbsGroupMembershipStore {
public:
    explicit MbsGroupMembershipStore(const std::string& conninfo);

    void put(const std::string& ext_group_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ext_group_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ext_group_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ext_group_id);
    // Real Query5GmbsGroup (ADR-0167) -- every persisted row, keyed by ext_group_id, for the bare
    // collection GET to compose into its own real map response.
    std::vector<std::pair<std::string, nlohmann::json>> list_all();

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0146). Backs the real `group-data`
// Event Exposure Data for a group resource (`{ueGroupId}/ee-profile-data`, real spec operation
// `QueryGroupEEData` -- real GET-only, no create/update operation exists in the spec at all,
// schema `EeGroupProfileData`). Genuinely NOT per-UE -- keyed by `ueGroupId` (real schema
// `VarUeGroupId`, a plain string), a real, distinct sibling of the already-closed per-UE
// `ee-profile-data` resource. Same "seed at startup, no live provisioning path" shape as
// `SponsorConnectivityDataStore`/`PlmnUePolicySetStore`.
class GroupEeProfileDataStore {
public:
    explicit GroupEeProfileDataStore(const std::string& conninfo);

    void seed(const std::string& ue_group_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_group_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0148). Backs the real Event
// Exposure Subscriptions collection + individual document resource (`context-data/
// ee-subscriptions` / `context-data/ee-subscriptions/{subsId}`, real spec operations
// `Queryeesubscriptions`/`CreateEeSubscriptions`/`QueryeeSubscription`/`UpdateEesubscriptions`/
// `ModifyEesubscription`/`RemoveeeSubscriptions`, schema `EeSubscription`). Real, disclosed: the
// caller (main.cpp) generates a real UUID v4 `subsId` via `sbi_core::generate_uuid_v4()` (same
// generator this project's own NF instance IDs use) and passes it to `create()` -- no
// client-supplied ID exists for this resource. `update()` is genuinely update-only, never create
// (real spec 404 for a nonexistent resource) -- a real, disclosed departure from every other
// single-key PUT resource this project has closed. `apply_patch` is real RFC 6902, NOT
// upsert-capable. `list()` backs the collection GET, deliberately not honoring the real,
// genuinely-optional `event-types`/`nf-identifiers` array filters (same "optional filter not
// honored" precedent as `RangingSlPrivacyDataStore`). Composite key (ue_id, subs_id).
class EeSubscriptionsStore {
public:
    explicit EeSubscriptionsStore(const std::string& conninfo);

    void create(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& subs_id);
    std::vector<nlohmann::json> list(const std::string& ue_id);
    bool update(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0149). Backs the real Subs To
// Notify collection (`subscription-data/subs-to-notify`, real spec operations
// `SubscriptionDataSubscriptions`/`QuerySubsToNotify`) and individual document
// (`subscription-data/subs-to-notify/{subsId}`, real spec operations
// `QuerySubscriptionDataSubscriptions`/`ModifysubscriptionDataSubscription`/
// `RemovesubscriptionDataSubscriptions` -- GET+PATCH+DELETE, genuinely no PUT). `subsId` is
// caller-generated (same precedent as `EeSubscriptionsStore`). `list_by_ue_id` backs the real,
// required `ue-id` query-param filter on the collection GET. `apply_patch` is real RFC 6902, NOT
// upsert-capable.
//
// Schema fix (ADR-0176, gap-closure task #106): `ue_id` is now a real nullable column/parameter
// (was a `NOT NULL` column backed by an empty-string sentinel for UE-less subscriptions) --
// `SubscriptionDataSubscriptions.ueId` is genuinely OPTIONAL per its own real schema.
// `list_ue_less()` (new) backs real `onDataChange` delivery for non-per-UE resources, which have
// no `ueId` to match `list_by_ue_id` against.
class SubsToNotifyStore {
public:
    explicit SubsToNotifyStore(const std::string& conninfo);

    void create(const std::string& subs_id,
                const std::optional<std::string>& ue_id,
                nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& subs_id);
    std::vector<nlohmann::json> list_by_ue_id(const std::string& ue_id);
    std::vector<nlohmann::json> list_ue_less();
    std::optional<nlohmann::json> apply_patch(const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0151). Backs the real SDM
// Subscriptions collection (`context-data/sdm-subscriptions`) and individual document
// (`context-data/sdm-subscriptions/{subsId}`, real spec operations
// `Querysdmsubscriptions`/`CreateSdmSubscriptions`/`QuerysdmSubscription`/
// `Updatesdmsubscriptions`/`ModifysdmSubscription`/`RemovesdmSubscriptions`, schema
// `SdmSubscription`). Structurally identical to `EeSubscriptionsStore` (ADR-0148):
// server-generated `subsId` (caller-generated, passed to `create()`), `update()` genuinely
// update-only never create (real spec 404 for a nonexistent resource), `apply_patch` real RFC
// 6902 NOT upsert-capable. Composite key (ue_id, subs_id).
class SdmSubscriptionsStore {
public:
    explicit SdmSubscriptionsStore(const std::string& conninfo);

    void create(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& subs_id);
    std::vector<nlohmann::json> list(const std::string& ue_id);
    bool update(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0152). Backs the real AMF
// Subscription Info (Document) resource nested under an individual ee-subscription
// (`context-data/ee-subscriptions/{subsId}/amf-subscriptions`, real spec operations
// `Create AMF Subscriptions`/`GetAmfSubscriptionInfo`/`ModifyAmfSubscriptionInfo`/
// `RemoveAmfSubscriptionsInfo`). Real, disclosed: the document body is a JSON ARRAY of
// `AmfSubscriptionInfo` (not a single object) -- stored and returned as one JSONB array value per
// (ue_id, subs_id), same as any other single-document store, just array-shaped at the JSON layer.
// `put()` returns `true` for a new entry (201-vs-204 selection, same precedent as
// `AmfContextStore`). No referential integrity enforced against `EeSubscriptionsStore` (this
// project's own established precedent of not enforcing cross-resource existence checks). First
// of `ee-subscriptions`' own nested sub-collections closed. Composite key (ue_id, subs_id).
class EeAmfSubscriptionInfoStore {
public:
    explicit EeAmfSubscriptionInfoStore(const std::string& conninfo);

    bool put(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& subs_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0153). Backs the real SMF Event
// Subscription Info (Document) resource nested under an individual ee-subscription
// (`context-data/ee-subscriptions/{subsId}/smf-subscriptions`, real spec operations
// `Create SMF Subscriptions`/`GetSmfSubscriptionInfo`/`ModifySmfSubscriptionInfo`/
// `RemoveSmfSubscriptionsInfo`). Real, disclosed: the document body is a SINGLE
// `SmfSubscriptionInfo` object (not an array, genuinely different from its sibling
// `EeAmfSubscriptionInfoStore`) -- structurally identical store shape otherwise (`put()` is-new
// tracking, same precedent). Second of `ee-subscriptions`' own nested sub-collections closed.
// Composite key (ue_id, subs_id).
class EeSmfSubscriptionInfoStore {
public:
    explicit EeSmfSubscriptionInfoStore(const std::string& conninfo);

    bool put(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& subs_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0154). Backs the real HSS Event
// Subscription Info (Document) resource nested under an individual ee-subscription
// (`context-data/ee-subscriptions/{subsId}/hss-subscriptions`, real spec operations
// `Create HSS Subscriptions`/`GetHssSubscriptionInfo`/`ModifyHssSubscriptionInfo`/
// `RemoveHssSubscriptionsInfo`). Real, disclosed spec inconsistency, asked and confirmed: the
// real spec's own `GetHssSubscriptionInfo` response schema literally cites `SmfSubscriptionInfo`,
// not `HssSubscriptionInfo` -- treated as a real typo, `get()` stores/returns real
// `HssSubscriptionInfo`-shaped data (wrapping `hssSubscriptionList`), matching this resource's own
// PUT/PATCH/DELETE and every sibling's internally-consistent pattern. Structurally identical
// store shape to `EeSmfSubscriptionInfoStore` otherwise. Third and final of `ee-subscriptions`'
// own nested sub-collections closed. Composite key (ue_id, subs_id).
class EeHssSubscriptionInfoStore {
public:
    explicit EeHssSubscriptionInfoStore(const std::string& conninfo);

    bool put(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& subs_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Real spec PUT documents only 204 (no 201) -- put() is void, always-upsert, same idiom as
// SorDataStore/UpuDataStore (ADR-0143), just with a composite key (ADR-0155).
class SdmHssSubscriptionInfoStore {
public:
    explicit SdmHssSubscriptionInfoStore(const std::string& conninfo);

    void put(const std::string& ue_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& subs_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Group-data-scoped sibling of EeSubscriptionsStore (ADR-0148), keyed by ue_group_id instead of
// ue_id -- otherwise identical shape (ADR-0156).
class GroupEeSubscriptionsStore {
public:
    explicit GroupEeSubscriptionsStore(const std::string& conninfo);

    void create(const std::string& ue_group_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_group_id, const std::string& subs_id);
    std::vector<nlohmann::json> list(const std::string& ue_group_id);
    bool update(const std::string& ue_group_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_group_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_group_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Group-data-scoped sibling of EeAmfSubscriptionInfoStore (ADR-0152), keyed by ue_group_id
// instead of ue_id -- otherwise identical shape (ADR-0157).
class GroupAmfSubscriptionInfoStore {
public:
    explicit GroupAmfSubscriptionInfoStore(const std::string& conninfo);

    bool put(const std::string& ue_group_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_group_id, const std::string& subs_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_group_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_group_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Group-data-scoped sibling of EeSmfSubscriptionInfoStore (ADR-0153), keyed by ue_group_id
// instead of ue_id -- otherwise identical shape (ADR-0158).
class GroupSmfSubscriptionInfoStore {
public:
    explicit GroupSmfSubscriptionInfoStore(const std::string& conninfo);

    bool put(const std::string& ue_group_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_group_id, const std::string& subs_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_group_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_group_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Group-data-scoped sibling of EeHssSubscriptionInfoStore (ADR-0154), keyed by ue_group_id
// instead of ue_id -- otherwise identical shape (ADR-0159).
class GroupHssSubscriptionInfoStore {
public:
    explicit GroupHssSubscriptionInfoStore(const std::string& conninfo);

    bool put(const std::string& ue_group_id, const std::string& subs_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_group_id, const std::string& subs_id);
    std::optional<nlohmann::json> apply_patch(const std::string& ue_group_id,
                                              const std::string& subs_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& ue_group_id, const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Real PDTQ Data (ADR-0162). Single-key on pdtq_ref_id (client-supplied, not server-generated).
// Real spec CreateIndividualPdtqData documents only 201, never 204 -- put() is void, matching
// BdtDataStore's own idiom.
class PdtqDataStore {
public:
    explicit PdtqDataStore(const std::string& conninfo);

    void put(const std::string& pdtq_ref_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& pdtq_ref_id);
    std::vector<nlohmann::json> list();
    // Real RFC 7396 JSON Merge Patch.
    std::optional<nlohmann::json> merge_patch(const std::string& pdtq_ref_id,
                                              const nlohmann::json& patch);
    bool remove(const std::string& pdtq_ref_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Real GetNfGroupIDs (Nudr_GroupIDmap, ADR-0164). No create/update operation exists anywhere in
// this service for the mapping data -- seed() + get() only, same shape as RoutingIdStore. Stores
// a plain string (real NfGroupId schema is `type: string`), not a JSON document.
class NfGroupIdStore {
public:
    explicit NfGroupIdStore(const std::string& conninfo);

    void seed(const std::string& subscriber_id, const std::string& nf_type, std::string group_id);
    std::optional<std::string> get(const std::string& subscriber_id, const std::string& nf_type);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Real GetNiddAuData (`/subscription-data/{ueId}/nidd-authorization-data`, ADR-0165). GET-only,
// no create/update/delete operation exists anywhere in this project's scope for this document
// (UDM's own Nudm_NIDDAU service, out of scope here, is the real provisioning path) -- seed() +
// get() only, same "no live provisioning path yet" precedent as RoutingIdStore/NfGroupIdStore.
// Keyed by (ueId, sst, sd, dnn, mtcProviderInformation) -- the resource's own real REQUIRED
// `single-nssai` (decomposed to sst/sd)/`dnn`/`mtc-provider-information` query-param filters,
// confirmed by direct YAML read. Real, disclosed: `Snssai`'s own schema only requires `sst`; `sd`
// is optional, represented here as an empty string when absent (documented, not a fabricated
// default) since PostgreSQL primary-key columns cannot be NULL.
class NiddAuthorizationDataStore {
public:
    explicit NiddAuthorizationDataStore(const std::string& conninfo);

    void seed(const std::string& ue_id,
              int sst,
              const std::string& sd,
              const std::string& dnn,
              const std::string& mtc_provider_information,
              nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id,
                                      int sst,
                                      const std::string& sd,
                                      const std::string& dnn,
                                      const std::string& mtc_provider_information);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Real Query5GVnGroupPPData/Query5GMbsGroupPPData (ADR-0169). Both are genuinely keyless
// singleton documents (confirmed by direct YAML read -- neither response schema is a per-group
// map, unlike every other `group-data` sub-resource in this series), backed by a fixed
// single-row table (id pinned to 1). No create/update/delete operation exists anywhere in the
// spec for either document -- seed() + get() only, same "no live provisioning path yet"
// precedent as RoutingIdStore/NfGroupIdStore.
class FiveGVnGroupPpProfileDataStore {
public:
    explicit FiveGVnGroupPpProfileDataStore(const std::string& conninfo);

    void seed(nlohmann::json data);
    std::optional<nlohmann::json> get();

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

class MbsGroupPpProfileDataStore {
public:
    explicit MbsGroupPpProfileDataStore(const std::string& conninfo);

    void seed(nlohmann::json data);
    std::optional<nlohmann::json> get();

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Real Nudr_GroupIDmap subscription-management family (ADR-0170) --
// CreateGroupIdSubscription/QueryGroupIdSubscription/ModifyGroupIdSubscription/
// RemoveGroupIdSubscription. Real POST+GET+PATCH+DELETE, server-generated subscriptionId. Real,
// disclosed: the spec's own `onGroupIdMapChange` webhook callback is not implemented here (no
// real outbound HTTP delivery) -- same disclosed gap class as SubsToNotifyStore's own lack of
// real webhook delivery.
class NfGroupIdSubscriptionStore {
public:
    explicit NfGroupIdSubscriptionStore(const std::string& conninfo);

    void create(const std::string& subscription_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& subscription_id);
    // Throws nlohmann::json::exception on a malformed patch -- caller turns that into a 400
    // ProblemDetails, same as every other apply_patch in this file.
    std::optional<nlohmann::json> apply_patch(const std::string& subscription_id,
                                              const nlohmann::json& patch_ops);
    bool remove(const std::string& subscription_id);
    // Real onGroupIdMapChange delivery (ADR-0180, gap-closure task #106). No `nfType`/`nfGroupId`
    // columns exist in the real table (the composite subscription key those two real
    // SubscriptionData fields form is stored only inside `data`) -- same "filter in the caller,
    // not in SQL" precedent already used for FiveGVnGroupStore::list_all()'s own `/internal`
    // filter route.
    std::vector<nlohmann::json> list_all();

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0182). Backs the real `policy-data`
// group's MBS Session Policy Control Data resource (`GetMBSSessPolCtrlData` -- real GET-only, no
// create/update operation exists in the spec at all). Keyed by `polSessionId`, but only ever the
// real `MbsSessPolDataId` schema's `afAppId` oneOf branch (a plain string, already unambiguous) --
// the schema's own `mbsSessionId`/`tmgi`/`ssm` branch remains genuinely unaddressed, per
// ADR-0119's own unreversed refusal to invent a serialization for it. See the full disclosure on
// `udr_mbs_session_pol_data` in `schema.postgres.sql`.
class MbsSessionPolicyDataStore {
public:
    explicit MbsSessionPolicyDataStore(const std::string& conninfo);

    void seed(const std::string& pol_session_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& pol_session_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0213). Backs the real
// `policy-data` group's Usage Monitoring Information (Document) resource
// (CreateUsageMonitoringResource/ReadUsageMonitoringInformation/DeleteUsageMonitoringInformation
// -- real PUT (create)+GET+DELETE per TS29519_Policy_Data.yaml), keyed by `(ueId, usageMonId)`.
// Real, disclosed: the spec's own GET documents a `204` ("resource found but no data") distinct
// from `404` ("resource not found") -- this project's single-row-per-key model has no
// exists-but-empty state, so an absent row is always the real `404`, not a fabricated `204`.
class UsageMonDataStore {
public:
    explicit UsageMonDataStore(const std::string& conninfo);

    void put(const std::string& ue_id, const std::string& usage_mon_id, nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& ue_id, const std::string& usage_mon_id);
    bool remove(const std::string& ue_id, const std::string& usage_mon_id);
    // Backs the bare `{ueId}` aggregate's own `umData` map field (keyed by `limitId`, the real
    // schema's own key -- confirmed equal to `usageMonId` in every real usage of this resource).
    std::vector<nlohmann::json> list_by_ue_id(const std::string& ue_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0213). Backs the real `policy-data`
// group's own Policy Data Subscriptions (Collection) + Individual Policy (Data) Subscription
// (Document) resources (CreateIndividualPolicyDataSubscription/ReadPolicyDataSubscriptions/
// ReadIndividualPolicySubscriptionData/ReplaceIndividualPolicyDataSubscription/
// DeleteIndividualPolicyDataSubscription) -- genuinely distinct real resource from
// `Subscription_Data.yaml`'s own `subs-to-notify` (`SubsToNotifyStore` above): different real
// schema (`PolicyDataSubscription`, not `SubscriptionDataSubscriptions`), different real
// individual-modify verb (PUT full replace here, RFC 6902 PATCH there), and the real, optional
// collection-GET filters here are `mon-resources`+`ue-id` (both optional) rather than
// `Subscription_Data`'s own required `ue-id`. Real, disclosed: the spec's own
// `policyDataChangeNotification` webhook callback is not implemented -- same disclosed gap class
// as `SubsToNotifyStore`'s own pre-ADR-0171 state and `Nudr_GroupIDmap`'s own
// `onGroupIdMapChange`.
class PolicyDataSubsToNotifyStore {
public:
    explicit PolicyDataSubsToNotifyStore(const std::string& conninfo);

    void create(const std::string& subs_id,
                const std::optional<std::string>& ue_id,
                nlohmann::json data);
    std::optional<nlohmann::json> get(const std::string& subs_id);
    std::vector<nlohmann::json> list_all();
    // Real full replace (PUT), not a patch -- returns the replaced document, `std::nullopt` if
    // `subs_id` doesn't exist.
    std::optional<nlohmann::json> replace(const std::string& subs_id, nlohmann::json data);
    bool remove(const std::string& subs_id);

private:
    std::mutex mutex_;
    pqxx::connection conn_;
};

} // namespace udr
