#include "lua_envy_dep_util.h"

#include <mutex>

namespace envy {

std::optional<direct_dependency> find_direct_dependency(pkg *from,
                                                        std::string const &query) {
  std::optional<direct_dependency> best;

  // One node, one lock, no recursion: the whole answer is in this map.
  std::lock_guard const deps_lock(from->deps_mutex);
  for (auto const &[dep_id, dep_info] : from->dependencies) {
    if (!dep_info.p) { continue; }
    if (dep_id != query && !dep_info.p->key.matches(query)) { continue; }

    // Ties broken by identity so an unordered map still gives one answer.
    if (!best || dep_info.needed_by < best->needed_by ||
        (dep_info.needed_by == best->needed_by && dep_id < best->identity)) {
      best = direct_dependency{ dep_info.p, dep_id, dep_info.needed_by };
    }
  }

  return best;
}

}  // namespace envy
