#include "lua_phase_context.h"

#include "lua_envy.h"
#include "pkg.h"
#include "util.h"

#include <exception>
#include <string>

namespace envy {

phase_context const *lua_phase_context_get(sol::this_state ts) {
  sol::state_view lua{ ts.L };
  sol::object obj{ lua.registry()[ENVY_PHASE_CTX_RIDX] };
  if (!obj.valid() || obj.get_type() == sol::type::lua_nil) { return nullptr; }
  return static_cast<phase_context const *>(obj.as<void *>());
}

std::filesystem::path lua_phase_context_resolve(sol::this_state ts,
                                                std::filesystem::path const &path) {
  if (path.is_absolute()) { return path; }
  phase_context const *ctx{ lua_phase_context_get(ts) };
  if (ctx && ctx->run_dir) { return *ctx->run_dir / path; }
  if (ctx && ctx->p && ctx->p->lock) { return ctx->p->lock->stage_dir() / path; }
  return std::filesystem::current_path() / path;
}

std::vector<unsigned char> lua_phase_context_read_file(sol::this_state ts,
                                                       std::string_view path,
                                                       char const *fn) {
  std::filesystem::path const p{ lua_phase_context_resolve(ts, std::string{ path }) };
  try {
    return util_load_file(p);
  } catch (std::exception const &) { throw_runtime_error({ fn, ": cannot read ", p }); }
}

void lua_phase_context_write_file(sol::this_state ts,
                                  std::string_view path,
                                  std::string_view content,
                                  char const *fn,
                                  bool executable) {
  std::filesystem::path const p{ lua_phase_context_resolve(ts, std::string{ path }) };
  try {
    if (p.has_parent_path()) { std::filesystem::create_directories(p.parent_path()); }
    util_write_file(p, content, executable);
  } catch (std::exception const &e) {
    throw_runtime_error({ fn, ": cannot write ", p, ": ", e.what() });
  }
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
