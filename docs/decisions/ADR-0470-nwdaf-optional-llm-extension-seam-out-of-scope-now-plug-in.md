## ADR-0470: NWDAF optional LLM extension seam: out of scope now, plug-in contract fixed

**Date:** 2026-10-08. **Status:** Proposed (user direction 2026-10-08: "keep LLM out of scope, but we need a plug and play kind of architecture"; the contract below is awaiting the user's review before any code).

**Context.** NWDAF end to end needs no LLM. TS 23.288 analytics (NF load, abnormal behaviour, slice SLA) are numeric prediction and classification, served by the existing path: Kafka, feature store, Python training sidecar, ONNX, in-process C++ inference (docs/project-context/ai-pipelines.md, ADR-0369). CLAUDE.md guardrail 7 (the model informs, the deterministic engine decides) forbids a non-deterministic model in the analytics decision path, and the dependency rule requires OSI-approved open source (many open-weight LLM licences are not). The user still wants to attach any LLM later without reworking NWDAF.

**Decision.**
1. No LLM code, dependency, model weight or network call is added to nfs/nwdaf now. NWDAF behaviour with the seam absent is the only behaviour that exists.
2. The seam is a separate, optional service that only CONSUMES finished NWDAF outputs (analytics results, MLModelMonitor accuracy reports) over the existing SBI/Kafka surfaces, and never writes back into Nnwdaf_AnalyticsInfo, MLModelProvision or any charging/policy decision. Output is advisory text for operators (GUI, MCP agent layer), human-approved for any action.
3. Provider-neutral contract, to be owned by this project and defined in a schema before code: request = {task, structured NWDAF result, language}; response = {text, model id, provider id, latency}. Providers are adapters selected by config (`llm_provider`, default `none`; endpoint and model in config/<service>.json, never in a .cpp). Which wire protocol an adapter speaks (for example an OpenAI-compatible chat endpoint, or a local runtime) is NOT decided here and is not asserted to be verified.
4. Admission rules for any provider: OSI-approved licence checked at selection time; self-hosted (no managed API); subscriber identifiers stripped before the request (PII audit mandatory); NWDAF must pass its full test suite with the seam disabled.

**Not done (disclosed).** Nothing is implemented: no schema, no service, no adapter, no config key, no test. The contract fields in (3) are a proposal, not a derived specification; they have no 3GPP counterpart (no TS defines an LLM interface), so they are project-defined and will be marked as such in any generated header.

**Rejected alternatives.**
- Linking an LLM runtime into the NWDAF process: couples memory, build time and licence risk to a core NF, and breaks "NFs are independent".
- Letting an LLM produce or adjust analytics values: violates guardrail 7.
- Hardcoding one model or vendor: violates configuration-over-code and the plug-and-play requirement.
- Skipping the ADR and adding the seam when needed: the user asked for the framework to be fixed now so later integration is a config change.
