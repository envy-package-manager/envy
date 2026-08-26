# Trace System

Structured machinery-observability events for functional tests and production diagnostics. Distinct from logs (`tui::*`), which are human per-package narrative. Trace is never prose; it is orthogonal to log verbosity.

## Enabling

`--trace=file:<path>` writes JSONL; `--trace=stderr` writes human `key=value`; comma-separate for both. Bare `--trace` defaults to stderr. Enabling trace does not change the log level.

## Format

JSONL, one record per line. First record is `trace_start` carrying the schema version. Envelope keys on every record:

- `seq` — monotonic `uint64`, assigned at emit under the queue mutex; total causal order (sort by this, not `ts`).
- `ts` — emit-time UTC ISO-8601 ms (`2025-01-15T10:30:00.123Z`).
- `tid` — small sequential thread id.
- `event` — event name.
- `spec` — subject package identity; omitted when engine/command-scoped.

Phase fields serialize as names (`"phase":"fetch"`), not numbers. Schema version: **2**.

## Authoring

Single source of truth: `src/trace_events.def` (X-macro table). It generates the event structs, the `trace_event_t` variant's names, both serializers (JSON + human, via one field visitor—cannot drift), and the `trace-schema` dump. Emit with `ENVY_TRACE(event_name, spec_expr, .field = value, ...)`; guarded on `g_trace_enabled`.

To add an event: add it to `trace_events.def` and `trace_event_t` (in `trace.h`), bump the count sentinel in `src/trace_tests.cpp`, and mirror it in `functional_tests/trace_parser.py`'s `EVENT_REGISTRY`. `test_trace_schema.py` diffs the binary's `trace-schema` dump against the parser registry, so drift fails a test.

## Events

| event | fields |
|---|---|
| `trace_start` | schema:i64 |
| `spec_registered` | key:str |
| `dependency_added` | dependency:str, needed_by:phase |
| `phase_start` | phase:phase |
| `phase_complete` | phase:phase, duration_ms:i64 |
| `phase_blocked` | blocked_at_phase:phase, waiting_for:str, target_phase:phase |
| `phase_unblocked` | unblocked_at_phase:phase, dependency:str |
| `target_extended` | old_target:phase, new_target:phase |
| `pkg_outcome` | outcome:str, duration_ms:i64 |
| `cache_hit` | cache_key:str, pkg_path:str, fast_path:bool |
| `cache_miss` | cache_key:str |
| `lock_acquired` | lock_path:str, wait_duration_ms:i64 |
| `lock_released` | lock_path:str, hold_duration_ms:i64 |
| `lua_ctx_package_access` | target:str, current_phase:phase, needed_by:phase, allowed:bool, reason:str |
| `lua_ctx_product_access` | target:str, provider:str, current_phase:phase, needed_by:phase, allowed:bool, reason:str |
| `lua_ctx_loadenv_spec_access` | target:str, subpath:str, current_phase:phase, needed_by:phase, allowed:bool, reason:str |
| `depot_check` | sha:str, result:str (hit\|miss\|sha_mismatch) |
| `product_resolved` | product:str, provider:str, via:str (registry\|identity\|fallback) |
| `deploy_script` | product:str, platform:str, action:str (created\|updated\|unchanged\|removed) |
| `cache_entry_finalized` | entry_dir:str, disposition:str (completed\|purged_user_managed\|cleaned_failure\|kept_partial) |
| `download_start` | url:str, destination:str |
| `download_complete` | url:str, bytes:i64, duration_ms:i64 |
| `download_failed` | url:str, error:str |
| `download_retry` | url:str, attempt:i64, delay_ms:i64, reason:str (connect\|transfer\|timeout\|http_status), error:str |
| `download_skipped` | url:str, reason:str |
| `git_resolve` | url:str, ref:str, sha:str, method:str (sha\|ls-remote) |
| `extract_start` | archive:str, destination:str, strip_components:i64 |
| `extract_complete` | archive:str, files_extracted:i64, duration_ms:i64 |

Not covered: `bootstrap.cpp`, `bundle.cpp`, `aws_util.cpp` (see `future-enhancements.md`).
