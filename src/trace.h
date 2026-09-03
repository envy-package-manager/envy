#pragma once

#include "pkg_phase.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace envy {

// Trace = machinery observability: structured events describing scheduler,
// cache/lock, and IO behavior. Consumed by functional tests and diagnostics.
// Event definitions live in trace_events.def (single source of truth).

inline constexpr std::int64_t kTraceSchemaVersion{ 2 };

namespace trace_events {

#define ENVY_TRACE_FIELD_STR(f) std::string f;
#define ENVY_TRACE_FIELD_I64(f) std::int64_t f{};
#define ENVY_TRACE_FIELD_BOOL(f) bool f{};
#define ENVY_TRACE_FIELD_PHASE(f) pkg_phase f{};
#define ENVY_TRACE_EVENT(name, fields) \
  struct name { \
    fields \
  };
#include "trace_events.def"
#undef ENVY_TRACE_EVENT
#undef ENVY_TRACE_FIELD_STR
#undef ENVY_TRACE_FIELD_I64
#undef ENVY_TRACE_FIELD_BOOL
#undef ENVY_TRACE_FIELD_PHASE

}  // namespace trace_events

using trace_event_t = std::variant<trace_events::trace_start,
                                   trace_events::spec_registered,
                                   trace_events::dependency_added,
                                   trace_events::phase_start,
                                   trace_events::phase_complete,
                                   trace_events::phase_blocked,
                                   trace_events::phase_unblocked,
                                   trace_events::target_extended,
                                   trace_events::pkg_outcome,
                                   trace_events::manifest_resolved,
                                   trace_events::manifest_imported,
                                   trace_events::cache_hit,
                                   trace_events::cache_miss,
                                   trace_events::lock_acquired,
                                   trace_events::lock_released,
                                   trace_events::lua_ctx_package_access,
                                   trace_events::lua_ctx_product_access,
                                   trace_events::lua_ctx_loadenv_spec_access,
                                   trace_events::depot_check,
                                   trace_events::product_resolved,
                                   trace_events::deploy_script,
                                   trace_events::cache_entry_finalized,
                                   trace_events::download_start,
                                   trace_events::download_complete,
                                   trace_events::download_failed,
                                   trace_events::download_retry,
                                   trace_events::download_skipped,
                                   trace_events::git_resolve,
                                   trace_events::extract_start,
                                   trace_events::extract_complete>;

inline constexpr std::size_t kTraceEventCount{ 0
#define ENVY_TRACE_EVENT(name, fields) +1
#include "trace_events.def"
#undef ENVY_TRACE_EVENT
};

static_assert(std::variant_size_v<trace_event_t> == kTraceEventCount,
              "trace_events.def and trace_event_t are out of sync");

// Per-event name + field visitor, generated from trace_events.def. The JSON and
// human serializers both consume trace_event_for_each_field so they cannot drift.
#define ENVY_TRACE_FIELD_STR(f) fn(#f, e.f);
#define ENVY_TRACE_FIELD_I64(f) fn(#f, e.f);
#define ENVY_TRACE_FIELD_BOOL(f) fn(#f, e.f);
#define ENVY_TRACE_FIELD_PHASE(f) fn(#f, e.f);
#define ENVY_TRACE_EVENT(name, fields) \
  constexpr std::string_view trace_event_name_of(trace_events::name const &) { \
    return #name; \
  } \
  template <typename Fn> \
  void trace_event_for_each_field(trace_events::name const &e, Fn &&fn) { \
    fields \
  }
#include "trace_events.def"
#undef ENVY_TRACE_EVENT
#undef ENVY_TRACE_FIELD_STR
#undef ENVY_TRACE_FIELD_I64
#undef ENVY_TRACE_FIELD_BOOL
#undef ENVY_TRACE_FIELD_PHASE

// Envelope: every emitted event carries a monotonic sequence number (true causal
// order), an emit-time timestamp, a small sequential thread id, and the subject
// spec (empty = engine-scoped, omitted from output).
struct trace_record {
  std::uint64_t seq;
  std::chrono::system_clock::time_point ts;
  std::uint32_t tid;
  std::string spec;
  trace_event_t event;
};

std::string_view trace_event_name(trace_event_t const &event);
std::string trace_record_to_string(trace_record const &rec);  // human key=value
std::string trace_record_to_json(trace_record const &rec);    // JSONL

// Schema registry (for the trace-schema dump + registry-sync tests).
struct trace_event_schema {
  std::string_view name;
  std::vector<std::string> fields;  // "name:str|i64|bool|phase"
};
std::vector<trace_event_schema> trace_schema();

namespace tui {
extern bool g_trace_enabled;
void trace(std::string spec, trace_event_t event);

inline bool trace_enabled() { return g_trace_enabled; }
}  // namespace tui

struct phase_trace_scope {
  std::string spec;
  pkg_phase phase;
  std::chrono::steady_clock::time_point start;

  phase_trace_scope(std::string pkg_identity,
                    pkg_phase phase_value,
                    std::chrono::steady_clock::time_point start_time);
  ~phase_trace_scope();
};

}  // namespace envy

#define ENVY_TRACE(type, spec_expr, ...) \
  do { \
    if (::envy::tui::g_trace_enabled) [[unlikely]] { \
      ::envy::tui::trace((spec_expr), ::envy::trace_events::type{ __VA_ARGS__ }); \
    } \
  } while (0)
