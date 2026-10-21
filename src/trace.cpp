#include "trace.h"

#include "pkg_phase.h"
#include "util.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <type_traits>

namespace envy {

namespace {

std::tm make_utc_tm(std::time_t time) {
  std::tm result{};

#if defined(_WIN32)
  gmtime_s(&result, &time);
#else
  gmtime_r(&time, &result);
#endif

  return result;
}

std::string format_timestamp(std::chrono::system_clock::time_point tp) {
  auto const seconds{ std::chrono::time_point_cast<std::chrono::seconds>(tp) };
  auto const millis{
    std::chrono::duration_cast<std::chrono::milliseconds>(tp - seconds).count()
  };

  std::time_t const timestamp{ std::chrono::system_clock::to_time_t(seconds) };
  std::tm const utc_tm{ make_utc_tm(timestamp) };

  char base[32]{};
  if (std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc_tm) == 0) { return {}; }

  char out[40]{};
  std::snprintf(out, sizeof(out), "%s.%03dZ", base, static_cast<int>(millis));
  return out;
}

void append_json_kv(std::string &out, std::string_view key, std::string_view value) {
  out.push_back(',');
  out.push_back('"');
  out.append(key);
  out.append("\":\"");
  out.append(util_escape_json_string(value));
  out.push_back('"');
}

void append_json_kv(std::string &out, std::string_view key, std::int64_t value) {
  out.push_back(',');
  out.push_back('"');
  out.append(key);
  out.append("\":");
  out.append(std::to_string(value));
}

void append_json_kv(std::string &out, std::string_view key, bool value) {
  out.push_back(',');
  out.push_back('"');
  out.append(key);
  out.append("\":");
  out.append(value ? "true" : "false");
}

}  // namespace

phase_trace_scope::phase_trace_scope(std::string pkg_identity,
                                     pkg_phase phase_value,
                                     std::chrono::steady_clock::time_point start_time)
    : spec{ std::move(pkg_identity) }, phase{ phase_value }, start{ start_time } {
  ENVY_TRACE(phase_start, spec, .phase = phase);
}

phase_trace_scope::~phase_trace_scope() {
  auto const duration_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count() };
  ENVY_TRACE(phase_complete,
             spec,
             .phase = phase,
             .duration_ms = static_cast<std::int64_t>(duration_ms));
}

std::string_view trace_event_name(trace_event_t const &event) {
  return envy::visit([](auto const &e) { return trace_event_name_of(e); }, event);
}

std::string trace_record_to_string(trace_record const &rec) {
  std::string out;
  out.reserve(160);
  out.append(trace_event_name(rec.event));
  if (!rec.spec.empty()) {
    out.append(" spec=");
    out.append(rec.spec);
  }

  envy::visit(
      [&](auto const &e) {
        trace_event_for_each_field(e, [&](std::string_view key, auto const &value) {
          out.push_back(' ');
          out.append(key);
          out.push_back('=');
          using T = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<T, std::string>) {
            out.append(value);
          } else if constexpr (std::is_same_v<T, bool>) {
            out.append(value ? "true" : "false");
          } else if constexpr (std::is_same_v<T, pkg_phase>) {
            out.append(pkg_phase_name(value));
          } else {
            out.append(std::to_string(value));
          }
        });
      },
      rec.event);

  return out;
}

std::string trace_record_to_json(trace_record const &rec) {
  std::string out;
  out.reserve(256);

  out.append("{\"seq\":");
  out.append(std::to_string(rec.seq));
  out.append(",\"ts\":\"");
  out.append(format_timestamp(rec.ts));
  out.append("\",\"tid\":");
  out.append(std::to_string(rec.tid));
  out.append(",\"event\":\"");
  out.append(trace_event_name(rec.event));
  out.push_back('"');
  if (!rec.spec.empty()) { append_json_kv(out, "spec", rec.spec); }

  envy::visit(
      [&](auto const &e) {
        trace_event_for_each_field(e, [&](std::string_view key, auto const &value) {
          using T = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<T, pkg_phase>) {
            append_json_kv(out, key, pkg_phase_name(value));
          } else if constexpr (std::is_same_v<T, std::string>) {
            append_json_kv(out, key, std::string_view{ value });
          } else {
            append_json_kv(out, key, value);
          }
        });
      },
      rec.event);

  out.push_back('}');
  return out;
}

std::vector<trace_event_schema> trace_schema() {
  std::vector<trace_event_schema> out;
  out.reserve(kTraceEventCount);

  auto const add{ [&out](auto const &e, std::string_view name) {
    trace_event_schema schema{ .name = name, .fields = {} };
    trace_event_for_each_field(e, [&](std::string_view key, auto const &value) {
      using T = std::remove_cvref_t<decltype(value)>;
      char const *const type{ [] {
        if constexpr (std::is_same_v<T, std::string>) {
          return "str";
        } else if constexpr (std::is_same_v<T, bool>) {
          return "bool";
        } else if constexpr (std::is_same_v<T, pkg_phase>) {
          return "phase";
        } else {
          return "i64";
        }
      }() };
      schema.fields.push_back(std::string{ key } + ":" + type);
    });
    out.push_back(std::move(schema));
  } };

#define ENVY_TRACE_EVENT(name, fields) add(trace_events::name{}, #name);
#include "trace_events.def"
#undef ENVY_TRACE_EVENT

  return out;
}

}  // namespace envy
