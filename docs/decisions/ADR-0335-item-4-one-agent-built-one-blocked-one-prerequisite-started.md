## ADR-0335: item 4 -- one agent built, one blocked, one prerequisite started

**Date:** 2026-09-11. **Status:** accepted. Item 4 of the agent roadmap, and it does not divide
into "done".

The user asked for the network-analytics, technical/ops and care/retention agents to be completed
"as and when NWDAF and other capabilities are built". This ADR does the part that is possible now
and is explicit about the rest, because a checkmark on all three would be false.

### Technical / ops agent: built, and deliberately half an agent

`agents/ops-agent/` ships with `list_nf_instances` and `get_offering`, and **no subscriber tools at
all** — an ops agent has no business reading a customer's balance, which the server enforces rather
than the prompt requesting.

Its operating rules forbid the thing it would otherwise do well: **report inventory, do not
diagnose.** "Three UDMs are registered" is an observation; "the network is healthy" is a diagnosis
it cannot support. An agent handed live NF state and asked "is anything wrong?" will produce a
confident answer out of nothing, and an operator cannot tell that from a real one. When asked an
analytics question it must name NWDAF as missing and stop — naming a missing capability is a useful
answer; improvising around it is not.

### Network-analytics agent: not built, and not fakeable

It needs NWDAF. There is no `nfs/nwdaf` directory. NEF's `AnalyticsExposure` and
`ReportingNetworkStatus` already answer 501 for the same reason (ADR-0324). Building this agent
means building NWDAF, and nothing short of that produces a real one.

### Care/retention agent: its prerequisite is now running

The blocker was checked rather than restated: `subscriber` had `created_at`, `updated_at` and **no
status column and no termination record**. A subscriber who leaves was either still present or
deleted — in neither case was there a record that they churned, when, or why. Churn propensity was
not hard here, it was **impossible: there was no label to train against.**

So the deliverable is the collection step, which can only gather history forward in time:
`subscriber.status` plus a `subscriber_lifecycle_event` table recording **both sides** of every
transition. Both sides matter — `active -> terminated` is a customer walking away,
`suspended -> terminated` is a collections outcome, and a model that cannot separate them learns
the wrong thing. `from_status` is read inside the transaction rather than supplied by the caller,
so it cannot be misreported, and the status update and the event are written atomically: a status
without its event, or an event without the status change, is a history that disagrees with the
record it describes.

`reason` is free text on purpose. No specification enumerates churn reasons, and inventing a closed
set now would force every future reason into a category chosen before anyone had seen the data.

### Two self-inflicted defects worth recording

**The `store.hpp` include collision, hit a second time.** Adding
`bss/subscriber-management/src` to `integration_tests`' include path silently redirected every bare
`#include "store.hpp"` in that directory, breaking `test_product_catalog_postgres.cpp` — a file
this change never touched. The identical trap was hit earlier in this project with
`bss/balance-management`. Fixed with an explicit relative include and a comment naming the trap, so
the next person does not re-add the path.

**A test that could only pass once.** `std::rand()` without a seed returns the same sequence in
every process, so the lifecycle test generated an identical SUPI on each run: green against a fresh
database, then permanently red on the UNIQUE constraint. It reported 20/20 on its first run, which
is exactly how this kind of defect survives review. Now keyed on time plus pid and verified across
three consecutive runs.

