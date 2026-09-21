-- =====================================================================================
-- CDR MEDIATION -- PostgreSQL control plane (relational, low-volume).
--
-- The Doris side (00-doris-mediation.sql) carries the CDR VOLUME. This side carries the
-- CONTROL state a mediation system needs and that must be transactional/relational:
--   * mediation.source       -- the source-system registry: format, service class, and
--                               crucially whether the feed is LIVE or only PROVISIONED.
--   * mediation.ingest_file   -- per-file provenance + idempotency (a file loads once).
--   * mediation.recon_run     -- one row per reconciliation run: window, counts, and the
--                               headline leakage number the RA team reads.
--
-- Own DB scope (mediation is a distinct NF/subsystem -- project_db_per_domain_rule keeps
-- UDR/ADRF separate for the same reason). No credentials or connection strings here; the
-- service sources its DSN from config/mediation.json (no-hardcoded-config rule).
-- =====================================================================================

CREATE SCHEMA IF NOT EXISTS mediation;

SET search_path TO mediation;

-- Source-system registry. Seeded (below) with every Tier-1 CDR source; `is_live` is the
-- honest disclosure of which feeds actually deliver records in this deployment vs which are
-- provisioned so recon data CAN be loaded when a feed is wired. `format` names the codec the
-- ingest adapter uses -- only ASN1_BER/TAP3/DIAMETER_GY have a real vendored codec in-repo;
-- GENERIC_CSV is the provisioned staging format for sources whose stage-3 layout is not held.
CREATE TABLE IF NOT EXISTS source (
    source_system   VARCHAR(32)  PRIMARY KEY,                     -- CHF_5G_DATA, MSC_VOICE, ...
    service_class   VARCHAR(16)  NOT NULL,                        -- DATA | VOICE | SMS
    format          VARCHAR(24)  NOT NULL,                        -- ASN1_BER|TAP3|DIAMETER_GY|GENERIC_CSV|DORIS_INTERNAL
    is_live         BOOLEAN      NOT NULL DEFAULT FALSE,          -- TRUE = feed delivers records here today
    enabled         BOOLEAN      NOT NULL DEFAULT TRUE,           -- operator on/off switch
    description     TEXT         NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ  NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ  NOT NULL DEFAULT now(),
    CONSTRAINT ck_source_service_class CHECK (service_class IN ('DATA','VOICE','SMS')),
    CONSTRAINT ck_source_format
        CHECK (format IN ('ASN1_BER','TAP3','DIAMETER_GY','GENERIC_CSV','DORIS_INTERNAL'))
);

-- Per-file provenance + idempotency. A source file loads exactly once: sha256 is the content
-- identity, file_name the operational identity; either colliding means "already ingested".
CREATE TABLE IF NOT EXISTS ingest_file (
    file_id         BIGSERIAL    PRIMARY KEY,
    source_system   VARCHAR(32)  NOT NULL REFERENCES source(source_system),
    file_name       VARCHAR(512) NOT NULL,
    sha256          CHAR(64)     NOT NULL,
    record_count    BIGINT       NOT NULL DEFAULT 0,
    accepted_count  BIGINT       NOT NULL DEFAULT 0,               -- rows written to Doris
    rejected_count  BIGINT       NOT NULL DEFAULT 0,               -- rows that failed decode/resolve
    status          VARCHAR(16)  NOT NULL DEFAULT 'LOADED',        -- LOADED | FAILED | DUPLICATE
    ingested_at     TIMESTAMPTZ  NOT NULL DEFAULT now(),
    CONSTRAINT ck_ingest_status CHECK (status IN ('LOADED','FAILED','DUPLICATE'))
);
-- Idempotency guards -- same file (by name or by content) is not double-loaded.
CREATE UNIQUE INDEX IF NOT EXISTS uq_ingest_file_name ON ingest_file(source_system, file_name);
CREATE UNIQUE INDEX IF NOT EXISTS uq_ingest_file_sha  ON ingest_file(sha256);
CREATE INDEX IF NOT EXISTS idx_ingest_file_source ON ingest_file(source_system, ingested_at);

-- One row per reconciliation run. The counts are the RA scorecard; leakage_total_amount is the
-- headline number (sum of amount_delta over MISSING_IN_CHF + AMOUNT_MISMATCH for the window).
CREATE TABLE IF NOT EXISTS recon_run (
    run_id              VARCHAR(64)  PRIMARY KEY,                  -- application-assigned (ties to Doris recon_result.run_id)
    window_start        DATE         NOT NULL,                    -- inclusive usage date
    window_end          DATE         NOT NULL,                    -- inclusive usage date
    service_class       VARCHAR(16),                              -- NULL = all classes
    status              VARCHAR(16)  NOT NULL DEFAULT 'RUNNING',   -- RUNNING | COMPLETED | FAILED
    started_at          TIMESTAMPTZ  NOT NULL DEFAULT now(),
    finished_at         TIMESTAMPTZ,
    records_network     BIGINT       NOT NULL DEFAULT 0,
    records_chf         BIGINT       NOT NULL DEFAULT 0,
    matched             BIGINT       NOT NULL DEFAULT 0,
    missing_in_chf      BIGINT       NOT NULL DEFAULT 0,           -- leakage count
    missing_in_network  BIGINT       NOT NULL DEFAULT 0,
    amount_mismatch     BIGINT       NOT NULL DEFAULT 0,
    duplicates          BIGINT       NOT NULL DEFAULT 0,
    leakage_total_amount NUMERIC(18,6) NOT NULL DEFAULT 0,         -- the RA headline
    leakage_currency    VARCHAR(8)   NOT NULL DEFAULT '',
    CONSTRAINT ck_recon_status CHECK (status IN ('RUNNING','COMPLETED','FAILED')),
    CONSTRAINT ck_recon_window CHECK (window_end >= window_start),
    CONSTRAINT ck_recon_service_class
        CHECK (service_class IS NULL OR service_class IN ('DATA','VOICE','SMS'))
);
CREATE INDEX IF NOT EXISTS idx_recon_run_window ON recon_run(window_start, window_end);
CREATE INDEX IF NOT EXISTS idx_recon_run_status ON recon_run(status, started_at);

-- Seed the source registry. Honest live/provisioned disclosure:
--   * CHF_5G_DATA is LIVE -- it is the nfs/chf `cdr` Doris table this project already fills.
--   * TAP3_ROAM and PGW_GY_4G have REAL vendored codecs (libs/tap3-core, libs/diameter-core)
--     -> ingest is buildable; marked not-live until a feed is wired.
--   * MSC/IMS/SMSC/SMSF are PROVISIONED via GENERIC_CSV staging (no stage-3 layout held
--     offline; not fabricated) so recon data can be loaded now, wired to real codecs later.
INSERT INTO source (source_system, service_class, format, is_live, description) VALUES
    ('CHF_5G_DATA', 'DATA',  'DORIS_INTERNAL', TRUE,
        '5G converged charging data CDRs -- nfs/chf cdr table (TS 32.291/32.298). Live.'),
    ('PGW_GY_4G',   'DATA',  'DIAMETER_GY',    FALSE,
        '4G online-charging usage via Gy CCR/CCA (RFC 4006 / TS 32.299). Codec: libs/diameter-core. Provisioned.'),
    ('TAP3_ROAM',   'DATA',  'TAP3',           FALSE,
        'Inbound/outbound roaming usage (TAP3, TD.57). Codec: libs/tap3-core. Provisioned.'),
    ('MSC_VOICE',   'VOICE', 'GENERIC_CSV',    FALSE,
        'Circuit-switched voice CDRs (TS 32.250). Layout not held offline -> GENERIC_CSV staging. Provisioned.'),
    ('IMS_VOICE',   'VOICE', 'GENERIC_CSV',    FALSE,
        'IMS/VoLTE voice CDRs (TS 32.260). Layout not held offline -> GENERIC_CSV staging. Provisioned.'),
    ('SMSC_SMS',    'SMS',   'GENERIC_CSV',    FALSE,
        'Legacy SMSC SMS CDRs (TS 32.270). Layout not held offline -> GENERIC_CSV staging. Provisioned.'),
    ('SMSF_SMS',    'SMS',   'GENERIC_CSV',    FALSE,
        '5G SMSF SMS CDRs (TS 32.274). Layout not held offline -> GENERIC_CSV staging. Provisioned.')
ON CONFLICT (source_system) DO NOTHING;
