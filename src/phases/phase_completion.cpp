#include "phase_completion.h"

#include "engine.h"
#include "pkg.h"
#include "trace.h"
#include "tui.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <tuple>

namespace envy {

namespace {

std::string format_duration(std::int64_t duration_ms) {
  char buf[32]{};
  std::snprintf(buf, sizeof(buf), " (%.1fs)", static_cast<double>(duration_ms) / 1000.0);
  return buf;
}

}  // namespace

void run_completion_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::completion,
                                       std::chrono::steady_clock::now() };

  switch (p->type) {
    case pkg_type::CACHE_MANAGED: p->result_hash = p->canonical_identity_hash; break;
    case pkg_type::USER_MANAGED: p->result_hash = "user-managed"; break;
    case pkg_type::BUNDLE_ONLY: p->result_hash = "bundle"; break;
    case pkg_type::UNKNOWN: break;
  }

  // One outcome per package, computed once and sent to three sinks: the TTY
  // progress section's final text (no new scrollback), a non-TTY INFO line
  // (CI/logs), and a machine-stable pkg_outcome trace event. Derived from
  // was_cache_hit (set by check) and imported (set by import) — NOT p->lock,
  // which the install phase moves out, leaving it null for fresh builds too.
  auto const [kind, human, timed]{ [&]() -> std::tuple<char const *, std::string, bool> {
    if (p->type == pkg_type::USER_MANAGED) {
      return { "setup_complete", "setup complete", false };
    }
    // Bundles have no build/import path: they are fetched, already cached, or a
    // local directory consumed where it sits.
    if (p->type == pkg_type::BUNDLE_ONLY && p->bundle_in_situ) {
      return { "bundle_local", "local bundle", false };
    }
    if (p->was_cache_hit) { return { "cache_hit", "cache hit", false }; }
    if (p->type == pkg_type::BUNDLE_ONLY) { return { "bundle_fetched", "fetched", true }; }
    if (p->imported) { return { "imported", "imported from depot", true }; }
    return { "installed", "installed", true };
  }() };

  auto const duration_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - p->build_start)
                              .count() };

  // Build/import paths show wall-clock; a cache hit or no-op setup does not.
  std::string const section_text{ timed ? human + format_duration(duration_ms) : human };

  ENVY_TRACE(pkg_outcome,
             p->cfg->identity,
             .outcome = kind,
             .duration_ms = static_cast<std::int64_t>(duration_ms));

  if (p->tui_section && tui::section_has_content(p->tui_section)) {
    tui::section_set_content(
        p->tui_section,
        tui::section_frame{ .label = "[" + p->cfg->identity + "]",
                            .content = tui::static_text_data{ .text = section_text } });
    tui::section_set_complete(p->tui_section);
  }

  // Off a TTY there is no live section to carry the outcome, so emit it as an
  // INFO line (auto-prefixed "[identity]" by the ambient log context). On a TTY
  // the section above is the only per-package output — no duplicate scrollback.
  if (!tui::is_tty()) { tui::info("%s", section_text.c_str()); }
}

}  // namespace envy
