#include "lua_phase_context.h"

#include "lua_envy.h"
#include "pkg.h"

namespace envy {

phase_context const *lua_phase_context_get(sol::this_state ts) {
  sol::state_view lua{ ts.L };
  sol::object obj{ lua.registry()[ENVY_PHASE_CTX_RIDX] };
  if (!obj.valid() || obj.get_type() == sol::type::lua_nil) { return nullptr; }
  return static_cast<phase_context const *>(obj.as<void *>());
}

phase_context_guard::phase_context_guard(engine *eng,
                                         pkg *p,
                                         lua_State *L,
                                         std::optional<std::filesystem::path> run_dir,
                                         cache::scoped_entry_lock const *lock,
                                         std::optional<std::filesystem::path> env_root)
    : lua_state_{ L },
      ctx_{ .eng = eng,
            .p = p,
            .run_dir = std::move(run_dir),
            .lock = lock,
            .env_root = std::move(env_root) } {
  if (!lua_state_) { return; }

  sol::state_view lua{ lua_state_ };
  lua.registry()[ENVY_PHASE_CTX_RIDX] = static_cast<void *>(&ctx_);
}

phase_context_guard::~phase_context_guard() {
  if (!lua_state_) { return; }

  sol::state_view lua{ lua_state_ };
  lua.registry()[ENVY_PHASE_CTX_RIDX] = sol::lua_nil;
}

}  // namespace envy
