---
name: spec-lookup
description: Find an API shape, schema, enum or path in the R19 OpenAPI specs or a generated header without reading it whole. Use whenever a field name, type or path from specs/ or libs/*-generated is needed.
---

1. `grep -n "<SchemaName>" specs/5G_APIs-REL-19/<TS-file>.yaml` (or the generated header) to get line numbers.
2. Read only that range with `offset`/`limit`. Never Read a generated header or a spec YAML whole.
3. API root comes from `servers[0].url` in the YAML, never from the filename (ADR-0325). If the YAML lacks it, stop and ask; never invent a field.
