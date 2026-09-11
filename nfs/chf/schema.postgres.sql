-- nfs/chf's real PostgreSQL schema -- P4.5/ADR-0060 (E5, Rating Function): CHF's first PostgreSQL
-- connection (previously Redis/Valkey for E3 session state and Apache Doris for E4 CDRs only,
-- ADR-0192).
--
-- Real TMF678 AppliedCustomerBillingRate is E5's own real SID mapping (docs/DATA_MODEL.md), the
-- "rated" representation of a charging decision. `rating_decision` combines this project's own
-- disclosed project-internal audit fields (CHARGING_PROMPT.md principle 1: "same inputs -> same
-- charge, forever, provably" -- input_snapshot/rule_fired_id/ai_advisory) with the real TMF678
-- fields the decision is realized as (acbr_type/acbr_is_billed/acbr_tax_excluded/
-- acbr_tax_included) on the SAME row, same "project-internal wrapper around a real TM Forum
-- resource" pattern this project already used for E6's Bucket.
--
-- Wired into CHF's real rating engine (build_rating_grant, ADR-0048/0057, nfs/chf/src/main.cpp):
-- every real rating decision (a grant issued, or explicitly withheld) writes one row here.

CREATE SEQUENCE IF NOT EXISTS rating_decision_id_seq;

CREATE TABLE IF NOT EXISTS rating_decision (
    id                  TEXT PRIMARY KEY,
    usage_record_id     TEXT,           -- FK -> a future E4 UsageRecord row; nullable, no such
                                        -- table exists yet (E4 is Apache Doris CDRs, ADR-0192, not this table)
    tariff_id           TEXT,           -- ProductOfferingPrice.id (E2), the tariff that fired
    tariff_version      TEXT,           -- ProductOfferingPrice.version, pinned per principle 1
    rating_group        BIGINT,
    input_snapshot       JSONB NOT NULL DEFAULT '{}', -- usage + offering/price ids + timestamp;
                                        -- disclosed real gap: balance-at-decision-time is NOT
                                        -- captured (would need an extra bss/balance-management
                                        -- call on every rating decision -- not added this pass)
    rated_amount         NUMERIC(18, 6),
    currency             TEXT,
    rule_fired_id         TEXT,          -- which ProductOffering/ProductOfferingPrice fired
                                        -- (principle 2: "every charge is explainable")
    ai_advisory           JSONB,          -- nullable; populated by P4.8's real AiQuotaSizer when
                                        -- it actually adjusted this decision's grant (ADR-0074)
    decided_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Real TMF678 AppliedCustomerBillingRate fields (E5's own real SID mapping):
    acbr_type             TEXT,          -- appliedBillingCharge | appliedBillingCredit | appliedPenaltyCharge
    acbr_is_billed         BOOLEAN,       -- always false at write time -- this project has no real
                                        -- billing-cycle/invoice-generation process yet to flip it
    acbr_tax_excluded      NUMERIC(18, 6),
    acbr_tax_included      NUMERIC(18, 6)
);

-- P4.5/ADR-0060 (E8, Security): real audit trail. Same real, local-per-service architectural
-- resolution as bss/product-catalog's own audit_record table (see that file's own header for the
-- full disclosure) -- CHF owns this PostgreSQL database, so its RatingDecision audit trail lives
-- here too, same transaction as the real rating_decision write it records.
CREATE SEQUENCE IF NOT EXISTS audit_record_id_seq;

CREATE TABLE IF NOT EXISTS audit_record (
    id                TEXT PRIMARY KEY,
    entity_type       TEXT NOT NULL,   -- RATING_DECISION
    entity_id         TEXT NOT NULL,
    action            TEXT NOT NULL,   -- e.g. "ratingDecision.record"
    actor             TEXT NOT NULL,
    before_snapshot   JSONB,
    after_snapshot    JSONB,
    ai_advisory_ref   TEXT,
    recorded_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ADR-0347: indexes for the read paths that exist, sized for a real subscriber base.
--
-- `rating_decision` carried only its primary key. Every "why was I charged this?" lookup
-- (chf::RatingDecisionStore::find_by_charging_data_ref, ADR-0332, and the MCP explain_charge tool
-- built on it) filters on a JSON field inside `input_snapshot` and sorts by `decided_at` -- a
-- sequential scan plus a sort over the whole table. Harmless at lab size, an outage at the volume
-- this system is being built for: one rating decision per rated usage means this table grows with
-- the CDR stream, not with the subscriber count.
--
-- An EXPRESSION index, because the reference lives inside the JSON document rather than in a
-- column of its own. Adding a generated column instead would be faster still but would change the
-- table's shape for every existing reader; this is the smaller change that fixes the scan.
CREATE INDEX IF NOT EXISTS rating_decision_charging_data_ref_idx
    ON rating_decision ((input_snapshot->>'chargingDataRef'), decided_at DESC);

-- The bill run selects unbilled applied-charge rows for an account and period. Without this it is
-- a full scan of every rating decision ever made, every billing cycle.
CREATE INDEX IF NOT EXISTS rating_decision_unbilled_idx
    ON rating_decision (acbr_is_billed, decided_at)
    WHERE acbr_type IS NOT NULL;

-- NWDAF and analytics read by tariff and by time. A single composite serves "what did this tariff
-- earn over this window", which is the shape both revenue reporting and model training use.
CREATE INDEX IF NOT EXISTS rating_decision_tariff_time_idx
    ON rating_decision (tariff_id, decided_at DESC);
