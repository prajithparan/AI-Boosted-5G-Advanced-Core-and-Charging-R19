## ADR-0275: product-catalog joins nf_config -- task #109 closed

**Date:** 2026-09-04
**Status:** Closed (work pushed; last citing commit fe33a18 on origin/main, 2026-10-06). Accepted

### What changed

`bss/product-catalog` was the last component in the repository holding deployment endpoints in
source: `kPort = 7785`, `kMetricsBindAddress`, `kSelfBase`, and `database_conninfo()` -- this
repo's first `getenv`-with-a-literal-fallback, whose own comment called itself "a deliberate,
disclosed departure from the rest of this codebase's convention".

It now loads `config/product-catalog.json` through `nf_config`, the same mechanism every NF uses.
`PRODUCT_CATALOG_DATABASE_URL` keeps working unchanged -- `nf_config::require` applies the env
override itself, and CI and the compose files already set it. The new
`PRODUCT_CATALOG_ADVERTISED_IPV4` follows the same convention.

`kApiRoot` (`/tmf-api/productCatalogManagement/v4`) stays in source: it is TMF620's own defined
path, not configuration -- the same line ADR-0273/ADR-0274 drew for the 3GPP service paths.

The advertised base is built from the same `port` the listener binds plus `advertised_ipv4`, so
the two cannot disagree. That is the drift ADR-0274 found in AMF, avoided here by construction
rather than by remembering.

### Verified

```
(config)                                   starting ... on https://127.0.0.1:7785
PRODUCT_CATALOG_ADVERTISED_IPV4=10.9.9.9   starting ... on https://10.9.9.9:7785
```

The binary then aborts on this machine because no PostgreSQL is listening for that component --
**pre-existing and unrelated**, confirmed rather than assumed: the same abort happens on a build
of the unmodified source (`git stash`, rebuild, run). Its own tests already `GTEST_SKIP()`
without `TEST_POSTGRES_URL`.

### Task #109 closed

```
grep -rn '"https://127\.0\.0\.1:' nfs/ bss/ --include=*.cpp --include=*.hpp   ->  0
```

Across ADR-0273 (AMF's four), ADR-0274 (CHF's two, plus the host literal ADR-0273 itself left
behind), and this ADR (product-catalog's four), every deployment endpoint now comes from a config
file with an env override. What deliberately stays in source, in all three: 3GPP/TMF-defined API
roots, and this lab's PLMN/slice identity constants (`kMcc`/`kMnc`/`kSst`/`kSd`) -- neither is a
deployment endpoint.

---

