#include "phase_completion.h"

#include "engine.h"
#include "pkg.h"
#include "trace.h"
#include "tui_actions.h"

#include <chrono>
#include <string>
#include <tuple>

namespace envy {

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
    if (p->type == pkg_type::BUNDLE_ONLY) {
      return { "bundle_fetched", "installed", true };
    }
    if (p->imported) { return { "imported", "imported from depot", true }; }
    return { "installed", "installed", true };
  }() };

  auto const elapsed{ std::chrono::steady_clock::now() - p->build_start };
  auto const duration_ms{
    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
  };

  std::string const section_text{ [&] {
    // A package that vendored reports the copy: vendoring is the last phase to write, and
    // "installed" or "cache hit" is the payload's verdict, not the project tree's. Under
    // `envy vendor` the command says it instead, once per target and in order.
    auto const *plan{ eng.vendor() };
    if (p->vendor_wrote && !(plan && plan->command_reports)) { return p->vendor_outcome; }
    // Build/import paths show wall-clock; a cache hit or no-op setup does not.
    return timed ? tui_actions::timed_outcome(human, elapsed) : human;
  }() };

  ENVY_TRACE(pkg_outcome,
             p->cfg->identity,
             .outcome = kind,
             .duration_ms = static_cast<std::int64_t>(duration_ms));

  // A row is earned: `timed` is the outcomes that moved bytes, the two flags the rest.
  // None of them, and the package leaves nothing on screen -- a no-work run is silent.
  tui_actions::report_outcome(p->tui_section,
                              p->cfg->identity,
                              section_text,
                              timed || p->setup_ran || p->vendor_wrote,
                              p->display);
}

}  // namespace envy
