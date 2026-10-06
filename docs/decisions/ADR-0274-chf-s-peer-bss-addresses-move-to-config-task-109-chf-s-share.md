## ADR-0274: CHF's peer BSS addresses move to config (task #109, CHF's share)

**Date:** 2026-09-04
**Status:** Accepted

### What changed

`chf::product_catalog_base()` and `chf::balance_management_base()` returned in-source literals
(`https://127.0.0.1:7785` / `:7786`) whenever their env var was unset. Both now fall back to
`config/chf.json` (`product_catalog_base_url` / `balance_management_base_url`).

The `CHF_PRODUCT_CATALOG_BASE` / `CHF_BALANCE_MANAGEMENT_BASE` env vars are unchanged and still
win -- they are how existing integration tests point CHF at their own instances -- because
`nf_config::require` applies the env override itself. The two code paths collapse into one call
rather than a hand-written `getenv`-then-literal pair.

**Read once, not per call.** These sit on CHF's charging hot path: six call sites across offering
lookup, balance reserve and balance adjust. Re-reading a JSON file per HTTP request would be a real
cost for a value that cannot change while the process runs, so each is a function-local static.
`main()` deliberately touches both at startup and logs them, so a missing or misspelled key fails
fast at boot -- with `nf_config`'s resolved-path message -- rather than throwing out of a charging
request later.

### Verified both ways

```
(config only)                                  chf: product catalog at https://127.0.0.1:7785,
                                                    balance management at https://127.0.0.1:7786
CHF_PRODUCT_CATALOG_BASE=https://127.0.0.1:19999  chf: product catalog at https://127.0.0.1:19999,
                                                    balance management at https://127.0.0.1:7786
```

The config value is really read, the env override still wins, and it applies to only the endpoint
it names. 9/9 CHF tests pass (the three `ProductCatalogPostgresTest` cases skip themselves without
`TEST_POSTGRES_URL`, as they always have).

### Task #109 after this ADR

Closed for the **NFs** -- `grep -rn '"https://127\.0\.0\.1:' nfs/` returns nothing -- but only
after this ADR caught one that ADR-0273 had left behind. That ADR derived AMF's `self_base` as
`"https://127.0.0.1:" + std::to_string(port)`: the port came from config, the **host was still a
literal**. Worse, `main()` separately hardcoded the same address in the NRF profile's
`ipv4Addresses`, so the address AMF published to NRF and the one it put in its own callback URIs
were two independent literals that could drift.

Both now come from one new key, `advertised_ipv4` (env `AMF_ADVERTISED_IPV4`), threaded into
`run_nrf_lifecycle`. The port is still reused from `port` rather than given a key of its own, for
the reason ADR-0273 gave -- a separate self-port key would need hand-syncing with the listener's.
Recorded here rather than quietly fixed, because the ADR that introduced it claimed the retrofit
was complete and the grep is what showed otherwise.

**Still open, and deliberately not done here:** `bss/product-catalog/src/main.cpp` holds
`kPort = 7785`, `kMetricsBindAddress` and `kSelfBase`. That component has **no config file at all**
-- there is no `config/product-catalog.json` -- so retrofitting it is not the same one-key change
as CHF's: it means bootstrapping `nf_config` into a BSS binary and creating the file, including its
own `port`, which is a different unit of work from swapping a fallback literal. Recorded as the
remaining piece of task #109 rather than folded in silently.

---

