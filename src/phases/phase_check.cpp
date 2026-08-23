#include "phase_check.h"

#include "blake3_util.h"
#include "cache.h"
#include "engine.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "trace.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace envy {

namespace {

// Compute hash and perform cache lookup. The Lua state is locked only to read
// PLATFORM/ARCH and released before ensure_pkg: ensure_pkg acquires the cache
// entry's file lock, and holding p->lua across that would invert the lock order
// taken by later phases (cache lock, then p->lua).
cache::ensure_result compute_hash_and_lookup_cache(pkg *p) {
  // Compute hash including resolved weak/ref-only dependencies
  std::string const key_for_hash{ [&] {
    std::ostringstream key;
    key << p->cfg->format_key();
    {  // deps_mutex guards the dependency maps; keep the scope to just the read
      std::lock_guard const deps_lock(p->deps_mutex);
      for (auto const &wk : p->resolved_weak_dependency_keys) { key << '|' << wk; }
    }
    return key.str();
  }() };

  auto const digest{ blake3_hash(key_for_hash.data(), key_for_hash.size()) };
  p->canonical_identity_hash = util_bytes_to_hex(digest.data(), 32);
  std::string const hash_prefix{ util_bytes_to_hex(digest.data(), 8) };

  auto const [platform, arch]{ [&] {
    auto const lua_acc{ p->lua.lock() };
    sol::state_view lua{ *lua_acc };
    return std::pair{ lua["envy"]["PLATFORM"].get<std::string>(),
                      lua["envy"]["ARCH"].get<std::string>() };
  }() };

  return p->cache_ptr->ensure_pkg(p->cfg->identity,
                                  platform,
                                  arch,
                                  hash_prefix,
                                  tui_actions::lock_wait_spinner(p->tui_section,
                                                                 p->cfg->identity,
                                                                 p->cfg->identity));
}

}  // namespace

void run_check_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::pkg_check,
                                       std::chrono::steady_clock::now() };

  // User-managed packages do all their work in check-gated SETUP pairs; the
  // shared cache holds nothing for them, so there is nothing to look up.
  if (p->type != pkg_type::CACHE_MANAGED) { return; }

  auto cache_result{ compute_hash_and_lookup_cache(p) };

  if (cache_result.lock) {  // Cache miss
    p->lock = std::move(cache_result.lock);
    tui::debug("check: miss — building");
  } else {  // Cache hit - store pkg_path, no lock means subsequent phases skip
    p->pkg_path = cache_result.pkg_path;
    p->was_cache_hit = true;
    tui::debug("check: hit at %s — skipping build",
               cache_result.pkg_path.string().c_str());
  }
}

}  // namespace envy
