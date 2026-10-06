## ADR-0292: Phase 7 GUI stack is decided in Phase 7

**Date:** 2026-09-05
**Status:** Accepted (user decision)

The brief proposes React + JSON Forms **or** Dear ImGui + nlohmann/json and asks for one to be
recommended in Phase 7. **User decision, 2026-09-05: that choice is made during Phase 7, not
before.**

Recorded so it is not re-raised as an open blocker at every status check: it is not blocking, it is
scheduled. What Phase 7 inherits regardless of which stack wins is fixed by ADR-0289 -- every
product, tariff, quota, throttle and partner surface must be editable from the GUI -- and the one
piece of work that is stack-independent is already named there: PCF's `policy_counter_actions` and
NSACF's slice quotas are config files today and need a runtime write API before any GUI can edit
them.

