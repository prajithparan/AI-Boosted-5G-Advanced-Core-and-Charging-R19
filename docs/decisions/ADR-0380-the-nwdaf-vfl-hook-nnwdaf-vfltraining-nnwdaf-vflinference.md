## ADR-0380: The NWDAF VFL hook -- Nnwdaf_VFLTraining / Nnwdaf_VFLInference subscription surface

**Date:** 2026-09-19. **Status:** accepted (hook -- the SBI surface and subscription lifecycle;
the federated-training coordination is Phase D). The last piece of the NWDAF-completion set
CLAUDE.md names (the "vertical-federated-learning (VFL) hook").

**Decision.** The two VFL resource families have real R19 stage-3 YAML
(`TS29520_Nnwdaf_VFLTraining.yaml`, `TS29520_Nnwdaf_VFLInference.yaml`), so the hook is built
against them, not invented. `nfs/nwdaf/src/vfl_subscription_store.{hpp,cpp}` is an in-memory
subscription store (assign-id / get / replace / RFC 7386 merge-patch / remove), and a templated
`register_vfl_crud<SubT, PatchT>` wires the full subscription CRUD for both roots
(`/nnwdaf-vfltraining/v1`, `/nnwdaf-vflinference/v1`): POST /subscriptions (201 + Location),
GET/PUT/PATCH/DELETE /subscriptions/{id}, each bearer-checked like every other NWDAF route, parsing
the generated `VflTrainingSubs_Nnwdaf_VFLTraining` / `VflInferSub_Nnwdaf_VFLInference` bodies and
their `*Patch` types.

**Disclosed.** This is a *hook*: the subscription lifecycle is real and conformant to the YAML, but
the VFL coordination the subscriptions would drive -- multi-party training across MTLFs with model
exchange -- is Phase D and not implemented (the NWDAF main.cpp header already listed VFL as Phase
D). The store is in-memory (no cross-restart persistence, same lab simplification as the SMF's
EventSubscriptionStore). The VFL services are not yet added to the NRF `nfServices` registration --
the endpoints are served but not advertised for discovery (a follow-up).

**Rejected.** *Implementing the federated-training coordination now* -- a large research feature
well beyond a "hook"; the SBI seam is what CLAUDE.md asks for. *Fabricating the surface* -- not
needed, the YAML exists.

**Not in this set: the energy-efficiency analytic.** CLAUDE.md lists it as an R19 addition, but the
frozen R19 Nnwdaf SBI has **no** energy-efficiency `NwdafEvent` or output DTO (the 23-value enum has
none, and there is no energy YAML). TS 23.288 clause 6.16 energy analytics is OAM/TS 28.552-sourced,
off the service-based interface. Building it as an Nnwdaf analytic would require inventing an
eventId/field, which the project's #1 rule forbids -- so it is flagged, not fabricated (see the
`project_nwdaf_energy_gap` note). A faithful path (stage-2 computation from the SMF's
ENERGY_USAGE_DATA event + OAM, exposed via metrics rather than a fabricated Nnwdaf surface) awaits a
decision. **That decision was taken -- see ADR-0381, which builds it.**

