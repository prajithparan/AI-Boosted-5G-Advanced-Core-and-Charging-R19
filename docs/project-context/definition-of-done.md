# Definition of done (per NF)

Moved verbatim from CLAUDE.md (section "Definition of done (per NF)", full text); see `docs/optimization/MOVE_LOG.md`.

1. API generated from the R19 YAML, with source file + branch cited.
2. NRF registration/discovery/heartbeat working, OAuth2 token validated.
3. All mandatory TS 23.502 procedures for that NF implemented + tested.
4. `ProblemDetails` error handling per TS 29.500.
5. OpenTelemetry spans + Prometheus metrics emitted.
6. Conformance test against the generated OpenAPI schema (request AND
   response).
7. [Opening of item 7 (the `config:ro` mount rule, ADR-0453) is kept once, verbatim, in `CLAUDE.md` section "Definition of done (per NF)" -- not repeated here.] A Dockerfile `COPY`ing the config
   file into the image is not an acceptable substitute -- the bind
   mount is the one, only mechanism, so a config edit takes effect on
   restart without an image rebuild, same as every other runtime value
   in this project.
8. Spec-traceability doc entry: procedure -> TS clause -> source file ->
   test.
9. Architecture diagram and product/license table in `README.md` updated:
   the NF's box goes from dashed to solid, new datastores/products get a
   row with their real license (conventions in `docs/ARCHITECTURE.md`).
