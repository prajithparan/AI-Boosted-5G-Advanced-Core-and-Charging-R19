## PHASE0-NOTES: build validation outcome

Resolved during the "build and run locally" pass (2026-08-04), g++ 13.3 / vcpkg baseline
`f1d4bbc7`, against the actual installed package versions:

- **nghttp2 C API** (nghttp2 1.69.0): the classic API written against
  (`nghttp2_session_mem_recv`/`mem_send`, `nghttp2_data_provider`, `nghttp2_submit_response`)
  compiled and ran correctly as-is -- no `*2`/`nghttp2_ssize` migration needed.
- **nghttp2 CMake integration**: wrong on the first attempt. vcpkg's `nghttp2` port ships only a
  pkg-config file (`libnghttp2.pc`), not a CMake config -- `find_package(nghttp2 CONFIG REQUIRED)`
  / `nghttp2::nghttp2` do not exist. Fixed to `pkg_check_modules(nghttp2 REQUIRED IMPORTED_TARGET
  libnghttp2)` / `PkgConfig::nghttp2` in `libs/sbi-core/CMakeLists.txt`. See ADR-0004.
- **OpenTelemetry C++ SDK** (opentelemetry-cpp 1.28.0): header paths and factory API
  (`TracerProviderFactory`, `OtlpHttpExporterFactory`, `OStreamSpanExporterFactory`,
  `SimpleSpanProcessorFactory`/`BatchSpanProcessorFactory`) were correct as written. One real bug:
  constructing `opentelemetry::nostd::shared_ptr<TracerProvider>` directly from the
  `std::unique_ptr` returned by `TracerProviderFactory::Create` is ambiguous (two applicable
  constructor overloads). Fixed in `otel.cpp` by routing through an intermediate
  `std::shared_ptr<TracerProvider>` first.
- **Other vcpkg CMake target names** used in `libs/sbi-core/CMakeLists.txt` --
  `opentelemetry-cpp::trace`, `opentelemetry-cpp::ostream_span_exporter`,
  `opentelemetry-cpp::otlp_http_exporter`, `tl::expected`, `nlohmann_json::nlohmann_json`,
  `spdlog::spdlog`, `CURL::libcurl`, `Boost::system` -- all resolved correctly, no changes needed.
- **vcpkg manifest**: `catch2` and (transitively) `openssl` need `pkg-config` on the build host to
  compile from source; wasn't installed initially. `catch2` was dropped from the manifest instead
  (unused -- see amended ADR-0003); `pkg-config` was installed for `openssl`, which is a real
  dependency and couldn't be dropped.
- **Result**: full build green (`sbi_core`, `stub-nrf`, `hello-nf`, `integration_tests`), the
  `tests/integration` GoogleTest suite passes, and a manual run confirms the real behaviour: hello-nf
  obtains an OAuth2 token, registers (HTTP 201), heartbeats (HTTP 200), deregisters (HTTP 204), all
  over actual h2c HTTP/2 through the hand-rolled nghttp2 server, with OTel spans emitted for the
  register/heartbeat steps.
- **Not yet clean**: `clang-tidy` (now runnable locally, clang-18 installed) reports ~40 style-only
  warnings across the hand-written files -- `readability-identifier-length` (short names like `ms`,
  `it`, `pd`), `misc-include-cleaner` (missing direct includes for transitively-available symbols),
  `misc-const-correctness`, one `google-build-using-namespace`, one `bugprone-exception-escape` on
  each `main()` (no top-level try/catch around the NF lifecycle). None are errors --
  `WarningsAsErrors` is empty and CI's lint job won't fail on them -- but they're real, undone
  cleanup, not silently ignored. Left for an incremental pass rather than gold-plating Phase 0
  further before Phase 1 starts.

---

