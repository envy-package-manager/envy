#include "trace.h"

#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

envy::trace_record make_record(envy::trace_event_t event) {
  return envy::trace_record{ .seq = 7,
                             .ts = std::chrono::system_clock::time_point{},
                             .tid = 3,
                             .spec = "ns.pkg@v1",
                             .event = std::move(event) };
}

// Every event type must serialize to a well-formed JSON object carrying the
// envelope keys and its own name. Iterating the variant by index means a newly
// added event type is covered automatically; the static_assert below forces the
// expected-count sentinel to be revisited whenever the variant grows or shrinks.
template <std::size_t I>
void check_alternative_serializes() {
  using alternative = std::variant_alternative_t<I, envy::trace_event_t>;
  auto const rec{ make_record(
      envy::trace_event_t{ std::in_place_index<I>, alternative{} }) };

  auto const name{ envy::trace_event_name(rec.event) };
  CHECK_FALSE(name.empty());

  auto const json{ envy::trace_record_to_json(rec) };
  REQUIRE(json.size() >= 2);
  CHECK(json.front() == '{');
  CHECK(json.back() == '}');
  CHECK(json.find("\"seq\":7") != std::string::npos);
  CHECK(json.find("\"ts\":\"") != std::string::npos);
  CHECK(json.find("\"tid\":3") != std::string::npos);
  CHECK(json.find("\"spec\":\"ns.pkg@v1\"") != std::string::npos);
  CHECK(json.find(std::string{ "\"event\":\"" } + std::string(name) + "\"") !=
        std::string::npos);

  // Human form leads with the event name and carries the spec.
  auto const human{ envy::trace_record_to_string(rec) };
  CHECK(human.rfind(name, 0) == 0);
  CHECK(human.find("spec=ns.pkg@v1") != std::string::npos);
}

template <std::size_t... Is>
void check_all(std::index_sequence<Is...>) {
  (check_alternative_serializes<Is>(), ...);
}

}  // namespace

TEST_CASE("trace_record_to_json emits valid JSON for every event type") {
  static_assert(envy::kTraceEventCount == 28,
                "trace_event_t changed: confirm the new/removed event serializes and "
                "update this count");
  check_all(std::make_index_sequence<envy::kTraceEventCount>{});
}

TEST_CASE("trace_event_name is unique per event type") {
  std::vector<std::string> names;
  auto collect{ [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    (names.emplace_back(envy::trace_event_name(
         envy::trace_event_t{ std::in_place_index<Is>,
                              std::variant_alternative_t<Is, envy::trace_event_t>{} })),
     ...);
  } };
  collect(std::make_index_sequence<envy::kTraceEventCount>{});

  std::sort(names.begin(), names.end());
  CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
}

TEST_CASE("json and human serializers agree on field sets") {
  // Both serializers consume trace_event_for_each_field; verify each schema field
  // name appears in both renderings of a default-constructed event.
  auto const schemas{ envy::trace_schema() };
  CHECK(schemas.size() == envy::kTraceEventCount);

  auto check_fields{ [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    (
        [&] {
          using alternative = std::variant_alternative_t<Is, envy::trace_event_t>;
          auto const rec{ make_record(
              envy::trace_event_t{ std::in_place_index<Is>, alternative{} }) };
          auto const name{ envy::trace_event_name(rec.event) };
          auto const json{ envy::trace_record_to_json(rec) };
          auto const human{ envy::trace_record_to_string(rec) };

          auto const it{ std::find_if(schemas.begin(), schemas.end(), [&](auto const &s) {
            return s.name == name;
          }) };
          REQUIRE(it != schemas.end());
          for (auto const &field : it->fields) {
            auto const key{ field.substr(0, field.find(':')) };
            CHECK(json.find("\"" + key + "\":") != std::string::npos);
            CHECK(human.find(key + "=") != std::string::npos);
          }
        }(),
        ...);
  } };
  check_fields(std::make_index_sequence<envy::kTraceEventCount>{});
}

TEST_CASE("empty spec is omitted from output") {
  auto rec{ make_record(envy::trace_events::trace_start{ .schema = 2 }) };
  rec.spec.clear();
  auto const json{ envy::trace_record_to_json(rec) };
  CHECK(json.find("\"spec\"") == std::string::npos);
  CHECK(json.find("\"schema\":2") != std::string::npos);
  auto const human{ envy::trace_record_to_string(rec) };
  CHECK(human.find("spec=") == std::string::npos);
}

TEST_CASE("trace_record_to_json produces valid ISO8601 timestamps") {
  auto rec{ make_record(
      envy::trace_events::phase_start{ .phase = envy::pkg_phase::spec_fetch }) };
  rec.ts = std::chrono::system_clock::now();
  auto const json{ envy::trace_record_to_json(rec) };

  auto const ts_start{ json.find("\"ts\":\"") };
  REQUIRE(ts_start != std::string::npos);
  auto const ts_value_start{ ts_start + 6 };
  auto const ts_end{ json.find('"', ts_value_start) };
  REQUIRE(ts_end != std::string::npos);
  auto const timestamp{ json.substr(ts_value_start, ts_end - ts_value_start) };

  // YYYY-MM-DDTHH:MM:SS.sssZ (length 24)
  REQUIRE(timestamp.size() == 24);
  CHECK(timestamp[4] == '-');
  CHECK(timestamp[7] == '-');
  CHECK(timestamp[10] == 'T');
  CHECK(timestamp[13] == ':');
  CHECK(timestamp[16] == ':');
  CHECK(timestamp[19] == '.');
  CHECK(timestamp[23] == 'Z');
}

TEST_CASE("phase fields serialize as names") {
  auto const rec{ make_record(
      envy::trace_events::phase_start{ .phase = envy::pkg_phase::pkg_fetch }) };
  auto const json{ envy::trace_record_to_json(rec) };
  CHECK(json.find("\"phase\":\"fetch\"") != std::string::npos);
  CHECK(json.find("phase_num") == std::string::npos);
}
