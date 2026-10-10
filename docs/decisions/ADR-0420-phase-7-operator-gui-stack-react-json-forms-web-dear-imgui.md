## ADR-0420: Phase 7 operator GUI stack -- React + JSON Forms (web), Dear ImGui + ImPlot (local engineering console, later)

**Date:** 2026-09-26. **Status:** accepted (project owner's decision, recorded here). Resolves the
open Phase 7 stack question (CLAUDE.md "GUI" line; README Phase 7 row).

**Decision.** Two tracks, two audiences:
1. **React + JSON Forms** (`gui/web`) for the operator / provisioning / configuration GUI:
   remote-capable, schema-driven (every form is rendered from a JSON Schema DERIVED from the real
   API or DTO definitions, ADR-0421), validating input before it leaves the browser. Served by a
   C++ backend-for-frontend (`gui/bff`, ADR-0422), never directly by an NF.
2. **Dear ImGui + ImPlot** for a separate LOCAL engineering console (live NF metrics, NWDAF analytics
   plots, message/trace inspectors). **Not in this increment** -- the second, later track.

**Toolchain (minimal, all OSI):** React 19.3 (MIT), @jsonforms/core + react + vanilla-renderers
3.8.0 (MIT), TypeScript 5.9 (Apache-2.0, build only), Vite 8.3 (MIT, build only; pulls rolldown
MIT, lightningcss MPL-2.0, postcss MIT). AJV (MIT) arrives transitively via JSON Forms but is not
executed (ADR-0421). `@vitejs/plugin-react` is deliberately NOT used: Vite's built-in TSX transform
suffices, and the plugin's babel/browserslist chain would bring `caniuse-lite` (CC-BY-4.0, not an
OSI licence). `gui/web/scripts/check_licenses.py` gates the WHOLE lockfile (58 packages today: MIT
41, MPL-2.0 12, Apache-2.0 2, BSD-3-Clause 2, ISC 1) and is a ctest (`gui_npm_licenses_osi_only`).
TypeScript is the one sanctioned exception to the C/C++/Python-only rule (the GUI exception).

**Rejected.** *Material renderers (@jsonforms/material-renderers)* -- better nested-array UX but pulls
MUI + emotion for a first increment that needs three small custom renderers instead
(`gui/web/src/forms/renderers.tsx`). *Server-side rendered forms in C++* -- would re-implement a
schema-form engine. *Dear ImGui for the operator GUI* -- a native binary is not remote-capable for
shop agents; kept for the engineering console where it fits.

