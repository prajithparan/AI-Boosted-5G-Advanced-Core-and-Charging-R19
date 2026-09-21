-- Charging/BSS domain DB -- schema-per-NF (project_db_per_domain_rule). Loaded first.
-- Full faithful TM Forum SID model, normalized (3NF), Tier-1 scale, EXTENDABLE via SID
-- Characteristic child tables (new attributes = rows, not schema changes). Domain files 10-70
-- source in FK order. Provenance: TMF Open API v4 field sets as realized in libs/bss_sid
-- (each header cites the exact TMFxxx swagger). GB922 not held offline -> attributes anchored on
-- those cited TMF resources; no SID attribute invented (#1 rule).
CREATE SCHEMA IF NOT EXISTS party;
CREATE SCHEMA IF NOT EXISTS product_catalog;
CREATE SCHEMA IF NOT EXISTS subscriber_mgmt;
CREATE SCHEMA IF NOT EXISTS balance_mgmt;
CREATE SCHEMA IF NOT EXISTS chf_rating;
CREATE SCHEMA IF NOT EXISTS roaming;
