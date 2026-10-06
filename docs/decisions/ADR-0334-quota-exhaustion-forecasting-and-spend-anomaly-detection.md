## ADR-0334: quota-exhaustion forecasting and spend-anomaly detection

**Date:** 2026-09-11. **Status:** accepted. Item 3 of the agent roadmap.

Two analytics tools on the MCP server, both computed from the subscriber's **own** recorded CDRs.

### Neither is a trained model, and that is the decision

- **Time to exhaustion is `remaining / burn_rate`.** That is arithmetic. Wrapping a division in a
  regressor would add opacity without accuracy, and would turn a number an operator can check by
  hand into one they cannot.
- **A spend anomaly is a departure from the subscriber's own history**, measured in standard
  deviations. A per-subscriber baseline needs no global training set and cannot encode a
  population bias a shared model would.

Where a model genuinely helps is predicting the **future** rate rather than extrapolating the
observed one — which is exactly what CHF's quota-sizing regressor already does (ADR-0074). Wiring
its prediction in as an alternative basis is named as the next step rather than half-built,
because it first needs that model trained on real CDRs instead of the `synthetic_bootstrap` its
own training script honestly discloses it may fall back to. Shipping a forecast powered by a model
trained on generated data, and calling it AI, would be the kind of claim this project keeps
refusing to make.

### The refusal cases are the design

Every result carries its basis and sample count, so a thin projection is visibly thin. Specific
refusals, each of which is a fabricated fact if you get it wrong:

- **Too little history** → `computable: false` with a reason, never a number.
- **Records spanning no elapsed time** → refused. Dividing by a zero span would report *"0 hours
  remaining"*, a fabricated emergency that an agent would relay verbatim.
- **A flat baseline** (stddev zero) → the z-score is undefined; reporting infinity would be
  nonsense delivered as certainty. The comparison is made directly and the absence of a score is
  stated.
- **An already-exhausted allowance** → an answer, not an error.

Anomaly detection is deliberately **one-sided**: spending far *less* than usual is not bill shock,
and alerting on it trains an operator to ignore the alert that matters. The baseline **excludes the
window under test** — including it would drag the mean toward the spike and hide exactly what the
tool is looking for.

The z-threshold is the operator's input, defaulted but overridable: what counts as bill shock
differs by market and tariff, so it is not this code's call.

15/15 MCP tests, the majority of them asserting refusals rather than happy paths.

