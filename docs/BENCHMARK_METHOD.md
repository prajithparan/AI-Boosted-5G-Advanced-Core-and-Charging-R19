# free5GC comparison: method, fixed before any number was seen

ADR-0329. This file was written and committed **before the first comparison run**, deliberately.
ADR-0049 mandates that this project *exceed* free5GC, and a benchmark run under a mandate to win is
exactly where a thumb lands on the scale without anyone noticing. So the rules are recorded first.

## Commitments

1. **The result is published in whichever direction it lands.** If free5GC is faster, the ADR says
   free5GC is faster, in the same words it would have used the other way.
2. **No tuning of this project after seeing free5GC's numbers** without saying so explicitly in the
   ADR, including what was changed and what the number was before.
3. **Any configuration asymmetry that could not be removed is listed below**, not discovered later
   by a reader.

## Scope

NRF `SearchNFInstances` (NFDiscovery) only -- the path this project's own baseline (ADR-0263)
already measures, and the hottest SBI path in a real core. AMF registration and PDU session
establishment need UERANSIM and the `gtp5g` kernel module; installing a kernel module on the
developer's machine is out of scope, so those are simply not measured rather than approximated.

## What is held equal

| Axis | Setting |
|---|---|
| Transport | HTTP/2 over TLS on both. free5GC configured `scheme: https` rather than its default `http`. |
| Authorization | OAuth2 bearer validated per request on both. free5GC `oauth: true` (its default). |
| Query | `?target-nf-type=UDM&requester-nf-type=AMF`, byte-identical on both. |
| Registered profiles | One UDM and one AMF in each NRF, same fields, so discovery serialises comparable payloads. |
| Load generator | The same `tools/sbi-loadgen` binary drives both. |
| Placement | Load generator and system under test on one host, over loopback, for both. |
| Repeats | 3 runs per case; median and spread reported, not a single run. |
| Build | This project built **Release**. free5GC's published image is a release build. |

## Known asymmetries, stated rather than hidden

- **mTLS.** This project's NRF requires a client certificate; free5GC's does server-side TLS only.
  Handshake cost amortises across a run only if the load generator reuses connections -- verified
  explicitly, not assumed (see the connection-reuse check in the results).
- **MongoDB.** free5GC's NRF stores NF profiles in MongoDB; this project's holds them in process.
  This is a **design difference, not an unfairness**, and is not neutralised. But whether free5GC's
  discovery path performs a database round trip per request is determined and stated, so no reader
  assumes otherwise.
- **Token issuance.** free5GC requires the token requester to be a registered NF; this project's
  NRF does not. This affects obtaining a token, not the per-request discovery path being measured.
- **Go vs C++ runtime.** Not an asymmetry to correct -- it is part of what each system is.

## What would invalidate a run, and is checked

- A Debug build of this project. `CMAKE_BUILD_TYPE` is recorded in `environment.txt` for every run.
  **ADR-0263's existing baseline was measured on a Debug build and did not say so** -- that is what
  prompted this check existing at all.
- Any non-200 response. The run is refused rather than reported (the existing baseline script
  already does this, after a run once measured the 401 path at full speed).
- The load generator, not the server, being the bottleneck. Checked by running two generator
  processes at the same total concurrency and comparing aggregate throughput to one; if aggregate
  rises, closed-loop numbers above that point are the client's and are not reported as the
  server's.
- Anything else resident on the machine. `environment.txt` records it.
