# ADR-0392: postgres-chf hosts the `charging` DB's time-partitioned event tables
# (balance_mgmt.topup_balance/adjust_balance/reserve_balance, chf_rating.rating_decision/
# applied_customer_billing_rate) at the Tier-1 target of hundreds of millions of rows/day
# (project_tier1_scale_architecture). Their automatic partition maintenance is pg_partman
# (PostgreSQL License, OSI-approved; https://github.com/pgpartman/pg_partman), installed from the
# official PGDG apt repository -- already present in the upstream postgres:16 (Debian) image, the
# same source this project already trusts for the base Postgres package itself.
#
# Base image is the DEBIAN postgres:16, not postgres:16-alpine: postgresql-16-partman is a real,
# PGDG-published .deb; there is no Alpine (.apk) package for it, and building it from source would
# mean this project hand-verifying a third-party checksum instead of trusting the same signed apt
# repository every other PostgreSQL package here already comes from.
#
# Version pinned to what was verified present in the PGDG trixie-pgdg repo at the time this file was
# written (deploy/db/charging/70-partition-management.sql's own header records the exact behaviour
# read from this installed version's source) -- deliberately NOT "latest", so a future upstream
# pg_partman release cannot silently change create_parent()/run_maintenance() behaviour under this
# deployment. Bump this pin (and re-verify 70-partition-management.sql's assumptions against the new
# version's own installed SQL) as a deliberate, reviewed step, not automatically.
FROM postgres:16

ARG PG_PARTMAN_VERSION=5.5.0-1.pgdg13+1

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        "postgresql-16-partman=${PG_PARTMAN_VERSION}" \
    && rm -rf /var/lib/apt/lists/*
