#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "bss_sid/product.hpp"
#include "nf_config/pg_pool.hpp"

// Private to bss/product-catalog. Real PostgreSQL persistence (libpqxx) -- replaces this file's
// earlier in-memory-only std::unordered_map stores, per the user's explicit direction ("make sure
// proper top rated high speed DBs... is used for Data model and persistency") and
// docs/DATA_MODEL.md's E2 / docs/DECISIONS.md ADR-0053/ADR-0054 persistence decision: PostgreSQL,
// header fields as real columns, TMF620's array/nested fields as `jsonb` columns on the same row
// (see schema.sql). Disclosed, real limitation: one libpqxx `pqxx::connection` per store instance,
// serialized behind a mutex (libpqxx connections are not safe for concurrent use from multiple
// threads without external synchronization) -- same "one shared handle, one mutex" discipline this
// project already applied to sbi_core::http2::Client (ADR-0051), not a connection pool. A real
// connection pool is future work if/when this becomes a throughput bottleneck; not assumed to be
// one yet, since nothing has been benchmarked (ADR-0049's own standing disclosure).

namespace product_catalog {

class ProductOfferingStore {
public:
    // resource_url is the collection's own real TMF620 URL (e.g.
    // "https://host:port/tmf-api/productCatalogManagement/v4/productOffering"), used to build each
    // stored resource's real `href` -- a genuine TMF620 field, not this project's invention.
    // conninfo is a libpq connection string (e.g. "postgresql://user:pass@host:port/dbname"); the
    // caller owns sourcing it (see main.cpp -- PRODUCT_CATALOG_DATABASE_URL env var, never
    // hardcoded credentials).
    ProductOfferingStore(std::string resource_url,
                         const std::string& conninfo,
                         std::size_t pool_size);

    // Server always assigns a fresh id/href on create, overwriting any client-supplied value --
    // matching real REST resource-creation semantics (the server owns identity assignment for a
    // POST to a collection). Returns the assigned id.
    std::string create(bss_sid::ProductOffering offering);
    std::optional<bss_sid::ProductOffering> get(const std::string& id);
    std::vector<bss_sid::ProductOffering> list();
    bool remove(const std::string& id);

private:
    std::string resource_url_;
    nf_config::PgPool pool_;
};

class ProductOfferingPriceStore {
public:
    ProductOfferingPriceStore(std::string resource_url,
                              const std::string& conninfo,
                              std::size_t pool_size);

    std::string create(bss_sid::ProductOfferingPrice price);
    std::optional<bss_sid::ProductOfferingPrice> get(const std::string& id);
    std::vector<bss_sid::ProductOfferingPrice> list();
    bool remove(const std::string& id);

private:
    std::string resource_url_;
    nf_config::PgPool pool_;
};

// New resource, added alongside the two above per docs/DATA_MODEL.md's E2 / the already-approved
// TMF620 extension scope: ProductSpecification -- what a ProductOffering.productSpecification
// references for its underlying definition, including configurable characteristics
// (productSpecCharacteristic / prodSpecCharValueUse -- see product.hpp's own header comment).
class ProductSpecificationStore {
public:
    ProductSpecificationStore(std::string resource_url,
                              const std::string& conninfo,
                              std::size_t pool_size);

    std::string create(bss_sid::ProductSpecification spec);
    std::optional<bss_sid::ProductSpecification> get(const std::string& id);
    std::vector<bss_sid::ProductSpecification> list();
    bool remove(const std::string& id);

private:
    std::string resource_url_;
    nf_config::PgPool pool_;
};

} // namespace product_catalog
