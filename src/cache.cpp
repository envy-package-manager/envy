#include "cache.h"

#include "blake3_util.h"
#include "platform.h"
#include "trace.h"
#include "tui.h"
#include "util.h"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

using path = std::filesystem::path;

namespace envy {

namespace {

// Absolute, separator-consistent, and free of '.'/'..' segments. Callers compare these
// paths and print them; `operator/` alone leaves `C:\proj` / `out/.envy` mixed.
path normalized(path p) { return p.lexically_normal().make_preferred(); }

bool strictly_inside(path const &inner, path const &outer) {
  auto const rel{ inner.lexically_relative(outer) };
  return !rel.empty() && rel != "." && *rel.begin() != "..";
}

}  // namespace

std::optional<std::string> validate_project_relative_path(std::string_view value) {
  if (value.empty()) { return "must not be empty"; }

  if (value.find('~') != std::string_view::npos) {
    return "must not contain '~'; tilde expansion was removed";
  }
  if (value.find('$') != std::string_view::npos ||
      value.find('%') != std::string_view::npos) {
    return "must not contain '$' or '%'; variable expansion was removed";
  }
  if (value[0] == '/' || value[0] == '\\') {
    return "must be relative, with no leading separator";
  }
  if (value.size() >= 2 && value[1] == ':') { return "must not name a drive"; }

  // Split on both separators: the value is authored once and read on every platform, so a
  // backslash is a separator here even when std::filesystem would not treat it as one.
  for (size_t pos{ 0 }; pos <= value.size();) {
    auto const end{ value.find_first_of("/\\", pos) };
    auto const component{
      value.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos)
    };
    if (component.empty()) { return "must not contain an empty path component"; }
    if (component == "." || component == "..") {
      return "must not contain a '.' or '..' component";
    }
    if (end == std::string_view::npos) { break; }
    pos = end + 1;
  }

  return std::nullopt;
}

cache_mode resolve_cache_mode(bool local_marker,
                              bool shared_marker,
                              std::optional<cache_mode> declared,
                              bool has_cache_local) {
  if (local_marker && shared_marker) {
    throw std::runtime_error(std::string{ "cache: both '" } + kCacheLocalMarker +
                             "' and '" + kCacheSharedMarker +
                             "' exist; envy never writes both. Delete one.");
  }
  if (local_marker) { return cache_mode::LOCAL; }
  if (shared_marker) { return cache_mode::SHARED; }
  if (declared) { return *declared; }
  // Naming a local tree is the declaration: a cache-local that took a second directive to
  // activate would sit in a manifest doing nothing, which is the worse trap.
  return has_cache_local ? cache_mode::LOCAL : cache_mode::SHARED;
}

std::optional<path> resolve_state_dir(std::optional<std::string> const &state_dir,
                                      path const &manifest_dir) {
  if (manifest_dir.empty()) { return std::nullopt; }
  if (!state_dir) { return normalized(manifest_dir); }

  if (auto const bad{ validate_project_relative_path(*state_dir) }) {
    throw std::runtime_error("'@envy state-dir' " + *bad + ": '" + *state_dir + "'");
  }
  return normalized(manifest_dir / *state_dir);
}

char const *cache_root_tier_name(cache_root_tier tier) {
  switch (tier) {
    case cache_root_tier::CLI_OVERRIDE: return "--cache-root/ENVY_CACHE_ROOT";
    case cache_root_tier::MARKER: return "recorded by 'envy cache'";
    case cache_root_tier::DIRECTIVE: return "@envy cache-mode";
    case cache_root_tier::IMPLIED_LOCAL: return "@envy cache-local";
    case cache_root_tier::DEFAULT: return "default";
  }
  return "default";
}

void cache_announce_root_once(cache_root_resolution const &resolved,
                              std::optional<std::string> const &bin_dir) {
  // Keyed on packages/, not on the root: the pre-dispatch self-deploy in main.cpp creates
  // <root>/envy/<version>/ before any command runs, so a root-existence test would be
  // false by the time anything could report it. packages/ is created by the first cache
  // entry, so its absence is exactly "no packages have landed here yet" -- and it comes
  // back after a teardown or a mode switch, which is when the notice is useful again.
  if (std::filesystem::exists(resolved.root / "packages")) { return; }

  // Native name and separator, as cmd_init's "Next steps" already does: the launcher is
  // envy.bat on Windows, and `./tools/envy` is not something cmd.exe can run.
  auto const launcher{ [&] {
    std::filesystem::path p{ "." };
    p /= bin_dir ? *bin_dir : std::string{ "bin" };
    p /= (platform::native() == platform_id::WINDOWS) ? "envy.bat" : "envy";
    return p.make_preferred().string();
  }() };
  bool const shared{ resolved.mode == cache_mode::SHARED };

  // tui::info, not print_stdout: log lines go to stderr (see the drain in tui.cpp), which
  // keeps `envy cache --root` parseable, and `-q` correctly silences a courtesy notice.
  tui::info("caching packages in %s", resolved.root.string().c_str());
  tui::info(shared ? "  shared with your other envy projects; deleting this project will "
                     "not remove them"
                   : "  inside this project, so deleting it removes them");
  tui::info("  %s instead: %s cache --%s",
            shared ? "keep them in this project" : "share one cache across projects",
            launcher.c_str(),
            shared ? "local" : "shared");
}

namespace {

// Shared by resolve_cache_root and cache_root_for_mode so neither skips a validation the
// other performs.
struct project_trees {
  std::optional<path> state;
  path local;  // empty when no manifest was found
};

project_trees resolve_project_trees(cache_root_request const &req) {
  if (req.cache_local) {
    if (auto const bad{ validate_project_relative_path(*req.cache_local) }) {
      throw std::runtime_error("'@envy cache-local' " + *bad + ": '" + *req.cache_local +
                               "'");
    }
  }

  auto state{ resolve_state_dir(req.state_dir, req.manifest_dir) };
  auto local{ req.manifest_dir.empty()
                  ? path{}
                  : normalized(req.manifest_dir / (req.cache_local
                                                       ? *req.cache_local
                                                       : kDefaultCacheLocal)) };

  // Equal is the co-located teardown a project asks for by pointing both directives at one
  // tree. Strict nesting is the accident: markers under the cache root vanish with a cache
  // wipe, and a cache root under the state dir makes `state-dir` mean something else.
  if (req.state_dir && state && !local.empty() && *state != local) {
    if (strictly_inside(*state, local) || strictly_inside(local, *state)) {
      throw std::runtime_error(
          "'@envy state-dir' and '@envy cache-local' must not nest: '" + state->string() +
          "' vs '" + local.string() +
          "'. Point them at the same directory to keep "
          "the override markers with the cache tree.");
    }
  }

  return { std::move(state), std::move(local) };
}

path root_in_mode(path const &local_tree, cache_mode mode) {
  if (mode == cache_mode::LOCAL) {
    if (local_tree.empty()) {
      throw std::runtime_error(
          "cache: local mode needs a manifest directory to anchor to, and none was found");
    }
    return local_tree;
  }

  if (auto def{ platform::get_default_cache_root() }) { return normalized(*def); }
  throw std::runtime_error("cannot determine cache root");
}

// Absolute only -- the binary used to absolutize against its own cwd while the launchers
// took it verbatim. SHARED always, so no caller pairs it with another mode.
std::optional<cache_root_resolution> override_resolution(cache_root_request const &req) {
  if (!req.cli_override) { return std::nullopt; }
  if (!req.cli_override->is_absolute()) {
    throw std::runtime_error("cache root override must be an absolute path: '" +
                             req.cli_override->string() + "'");
  }
  return cache_root_resolution{ normalized(*req.cli_override),
                                cache_mode::SHARED,
                                cache_root_tier::CLI_OVERRIDE };
}

}  // namespace

cache_root_resolution resolve_cache_root(cache_root_request const &req) {
  if (auto ovr{ override_resolution(req) }) { return std::move(*ovr); }

  auto const trees{ resolve_project_trees(req) };

  bool local_marker{ false }, shared_marker{ false };
  if (trees.state) {
    local_marker = platform::file_exists(*trees.state / kCacheLocalMarker);
    shared_marker = platform::file_exists(*trees.state / kCacheSharedMarker);
  }

  auto const mode{ resolve_cache_mode(local_marker,
                                      shared_marker,
                                      req.declared_mode,
                                      req.cache_local.has_value()) };

  auto const tier{ [&] {
    if (local_marker || shared_marker) { return cache_root_tier::MARKER; }
    if (req.declared_mode) { return cache_root_tier::DIRECTIVE; }
    if (req.cache_local) { return cache_root_tier::IMPLIED_LOCAL; }
    return cache_root_tier::DEFAULT;
  }() };

  return { root_in_mode(trees.local, mode), mode, tier };
}

cache_root_resolution cache_root_for_mode(cache_root_request const &req, cache_mode mode) {
  // `mode` is what the caller is about to record; an override outranks it.
  if (auto ovr{ override_resolution(req) }) { return std::move(*ovr); }

  // MARKER: the caller is about to write one, and no other tier yields a mode the manifest
  // does not already imply.
  return { root_in_mode(resolve_project_trees(req).local, mode),
           mode,
           cache_root_tier::MARKER };
}

std::optional<path> resolve_user_wide_cache_root(std::optional<path> const &cli_override) {
  // Same function as every other tier: rejected here but accepted there is two answers.
  if (auto ovr{ override_resolution({ .cli_override = cli_override }) }) {
    return std::move(ovr->root);
  }

  // nullopt, not a throw: a HOME-less box still runs a project whose cache is all in-tree.
  if (auto def{ platform::get_default_cache_root() }) { return normalized(*def); }
  return std::nullopt;
}

std::vector<path> envy_binary_candidates(cache_root_resolution const &resolved,
                                         std::optional<path> const &user_wide_root,
                                         std::string_view version,
                                         bool has_sums_pin) {
  path const rel{ path{ "envy" } / std::string{ version } / platform::exe_name("envy") };
  std::vector<path> out{ resolved.root / rel };

  // Keyed on the tier, not the mode: an override reports SHARED, so a mode test would read
  // backwards. An explicit root names one tree and must not be widened to two.
  if (resolved.tier == cache_root_tier::CLI_OVERRIDE ||
      resolved.mode != cache_mode::LOCAL || has_sums_pin || !user_wide_root) {
    return out;
  }

  if (auto candidate{ *user_wide_root / rel }; candidate != out.front()) {
    out.push_back(std::move(candidate));
  }
  return out;
}

struct cache_impl {
  path root_;

  path specs_dir() const { return root_ / "specs"; }
  path packages_dir() const { return root_ / "packages"; }
  path locks_dir() const { return root_ / "locks"; }
};

struct cache::impl : cache_impl {};

struct cache::scoped_entry_lock::impl {
  path entry_dir_;
  platform::file_lock lock_;
  path lock_path_;
  std::string pkg_identity_;
  std::chrono::steady_clock::time_point lock_acquired_time{};
  bool completed_{ false };
  bool user_managed_{ false };
  bool preserve_fetch_{ false };

  impl(path entry_dir,
       platform::file_lock lock,
       path lock_path,
       std::string pkg_identity,
       std::chrono::steady_clock::time_point lock_acquired_at)
      : entry_dir_{ std::move(entry_dir) },
        lock_{ std::move(lock) },
        lock_path_{ std::move(lock_path) },
        pkg_identity_{ std::move(pkg_identity) },
        lock_acquired_time{ lock_acquired_at } {}
};

}  // namespace envy

namespace {

// Best-effort single-shot removal — all callers tolerate failure (constructor
// cleans up stale dirs on next run).  No retry; remove_all_with_retry's 3.5s
// backoff is wasted on ephemeral dirs that Defender may hold briefly.
void remove_all_noexcept(path const &target) {
  std::error_code ec;
  std::filesystem::remove_all(target, ec);
}

envy::cache::ensure_result ensure_entry(
    envy::cache_impl &impl,
    path const &entry_dir,
    path const &lock_path,
    std::string_view pkg_identity,
    std::string_view cache_key,
    envy::platform::file_lock::contended_cb_t const &on_lock_contended) {
  envy::cache::ensure_result result{ entry_dir, entry_dir / "pkg", nullptr };

  if (envy::cache::is_entry_complete(entry_dir)) {
    ENVY_TRACE(cache_hit,
               std::string(pkg_identity),
               .cache_key = std::string(cache_key),
               .pkg_path = result.pkg_path.string(),
               .fast_path = true);
    return result;
  }

  std::filesystem::create_directories(impl.locks_dir());
  std::filesystem::create_directories(entry_dir);

  auto const lock_wait_start{ std::chrono::steady_clock::now() };
  envy::platform::file_lock lock{ lock_path, on_lock_contended };
  auto const lock_acquired_at{ std::chrono::steady_clock::now() };
  auto const wait_duration_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(
                                   lock_acquired_at - lock_wait_start)
                                   .count() };
  ENVY_TRACE(lock_acquired,
             std::string(pkg_identity),
             .lock_path = lock_path.string(),
             .wait_duration_ms = static_cast<std::int64_t>(wait_duration_ms));

  if (envy::cache::is_entry_complete(entry_dir)) {
    ENVY_TRACE(cache_hit,
               std::string(pkg_identity),
               .cache_key = std::string(cache_key),
               .pkg_path = result.pkg_path.string(),
               .fast_path = false);
    // Coarse lock is released here (dtor) without handing off to a
    // scoped_entry_lock; pair the acquire so lock events balance.
    ENVY_TRACE(lock_released,
               std::string(pkg_identity),
               .lock_path = lock_path.string(),
               .hold_duration_ms = static_cast<std::int64_t>(
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - lock_acquired_at)
                       .count()));
    return result;
  }

  ENVY_TRACE(cache_miss, std::string(pkg_identity), .cache_key = std::string(cache_key));
  result.lock = envy::cache::scoped_entry_lock::make(entry_dir,
                                                     std::move(lock),
                                                     lock_path,
                                                     std::string{ pkg_identity },
                                                     lock_acquired_at);
  return result;
}

}  // namespace

namespace envy {

cache::scoped_entry_lock::scoped_entry_lock(
    path entry_dir,
    platform::file_lock lock,
    path lock_path,
    std::string pkg_identity,
    std::chrono::steady_clock::time_point lock_acquired_at)
    : m{ std::make_unique<impl>(std::move(entry_dir),
                                std::move(lock),
                                std::move(lock_path),
                                std::move(pkg_identity),
                                lock_acquired_at) } {
  remove_all_noexcept(install_dir());
  remove_all_noexcept(work_dir());  // always delete (purely ephemeral)

  // Preserve fetch/ to enable per-file caching across failed attempts; create
  // the rest.
  std::filesystem::create_directories(fetch_dir());
  std::filesystem::create_directories(install_dir());
  std::filesystem::create_directories(work_dir());
  platform::mark_not_indexed(work_dir());  // prevent Indexer from holding handles
  std::filesystem::create_directories(stage_dir());
  std::filesystem::create_directories(tmp_dir());
}

cache::scoped_entry_lock::~scoped_entry_lock() {
  auto const hold_duration_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() -
                                   m->lock_acquired_time)
                                   .count() };

  char const *disposition{ "kept_partial" };

  if (m->completed_) {
    disposition = "completed";
    remove_all_noexcept(work_dir());
    if (!m->preserve_fetch_) {
      // fetch_dir cleanup is best-effort: the install is already complete, so a
      // lingering fetch dir only wastes disk space.  On Windows, Defender or Search
      // Indexer may still be scanning recently-downloaded archives.
      if (auto ec{ platform::remove_all_with_retry(fetch_dir()) }) {
        tui::warn("cache: could not remove %s: %s",
                  fetch_dir().string().c_str(),
                  ec.message().c_str());
      }
    }
    platform::touch_file(m->entry_dir_ / "envy-complete");
    platform::flush_directory(m->entry_dir_);
  } else if (m->user_managed_) {
    disposition = "purged_user_managed";
    remove_all_noexcept(m->entry_dir_);
  } else {
    // Check empty install_dir AND fetch_dir (installation didn't use cache at all)
    std::error_code ec;

    bool const install_dir_empty{ [&] {  // Check install_dir
      std::filesystem::directory_iterator it{ install_dir(), ec };
      if (ec) {
        ec.clear();
        return false;  // Conservative assumption: treat as not empty if error
      }
      return it == std::filesystem::directory_iterator{};
    }() };

    bool const fetch_dir_empty{ [&] {  // Check fetch_dir
      std::filesystem::directory_iterator it{ fetch_dir(), ec };
      if (ec) {
        ec.clear();
        return false;  // Conservative assumption: treat as not empty if error
      }
      return it == std::filesystem::directory_iterator{};
    }() };

    remove_all_noexcept(install_dir());
    remove_all_noexcept(work_dir());

    if (install_dir_empty && fetch_dir_empty) {
      disposition = "cleaned_failure";
      remove_all_noexcept(fetch_dir());
    }
  }

  ENVY_TRACE(cache_entry_finalized,
             m->pkg_identity_,
             .entry_dir = m->entry_dir_.string(),
             .disposition = disposition);
  ENVY_TRACE(lock_released,
             m->pkg_identity_,
             .lock_path = m->lock_path_.string(),
             .hold_duration_ms = static_cast<std::int64_t>(hold_duration_ms));
}

cache::scoped_entry_lock::ptr_t cache::scoped_entry_lock::make(
    path entry_dir,
    platform::file_lock lock_handle,
    path lock_path,
    std::string pkg_identity,
    std::chrono::steady_clock::time_point lock_acquired_at) {
  return ptr_t{ new scoped_entry_lock{ std::move(entry_dir),
                                       std::move(lock_handle),
                                       std::move(lock_path),
                                       std::move(pkg_identity),
                                       lock_acquired_at } };
}

cache::path cache::scoped_entry_lock::install_dir() const { return m->entry_dir_ / "pkg"; }

void cache::scoped_entry_lock::mark_install_complete() { m->completed_ = true; }
void cache::scoped_entry_lock::mark_user_managed() { m->user_managed_ = true; }
void cache::scoped_entry_lock::mark_preserve_fetch() { m->preserve_fetch_ = true; }

void cache::scoped_entry_lock::mark_fetch_complete() {
  std::filesystem::create_directories(fetch_dir());
  platform::touch_file(fetch_dir() / "envy-complete");
}

bool cache::scoped_entry_lock::is_install_complete() const { return m->completed_; }

bool cache::scoped_entry_lock::is_fetch_complete() const {
  return std::filesystem::exists(fetch_dir() / "envy-complete");
}

cache::path cache::scoped_entry_lock::stage_dir() const { return work_dir() / "stage"; }
cache::path cache::scoped_entry_lock::fetch_dir() const { return m->entry_dir_ / "fetch"; }
cache::path cache::scoped_entry_lock::work_dir() const { return m->entry_dir_ / "work"; }
cache::path cache::scoped_entry_lock::tmp_dir() const { return work_dir() / "tmp"; }

cache::cache(std::optional<path> root) : m{ std::make_unique<impl>() } {
  if (std::optional<path> maybe_root{ root ? root : platform::get_default_cache_root() }) {
    m->root_ = *maybe_root;
    return;
  }

  std::ostringstream oss;
  oss << "Unable to determine default cache root: "
      << platform::get_default_cache_root_env_vars() << " not set";
  throw std::runtime_error(oss.str());
}

cache::~cache() = default;

path const &cache::root() const { return m->root_; }

bool cache::is_entry_complete(path const &entry_dir) {
  return platform::file_exists(entry_dir / "envy-complete");
}

std::string cache::key(std::string_view identity,
                       std::string_view platform,
                       std::string_view arch,
                       std::string_view hash_prefix) {
  std::ostringstream s;
  s << identity << '-' << platform << '-' << arch << "-blake3-" << hash_prefix;
  return s.str();
}

path cache::compute_pkg_path(std::string_view identity,
                             std::string_view platform,
                             std::string_view arch,
                             std::string_view hash_prefix) const {
  std::ostringstream oss;
  oss << platform << '-' << arch << "-blake3-" << hash_prefix;
  return m->packages_dir() / std::string(identity) / oss.str() / "pkg";
}

cache::ensure_result cache::ensure_pkg(
    std::string_view identity,
    std::string_view platform,
    std::string_view arch,
    std::string_view hash_prefix,
    envy::platform::file_lock::contended_cb_t on_lock_contended) {
  if (!util_is_safe_path_component(identity)) {
    throw std::runtime_error("cache: invalid package identity: '" +
                             std::string{ identity } + "'");
  }
  path const pkg_path{ compute_pkg_path(identity, platform, arch, hash_prefix) };
  path const entry_dir{ pkg_path.parent_path() };
  auto const k{ key(identity, platform, arch, hash_prefix) };

  std::ostringstream lock_oss;
  lock_oss << "packages." << k << ".lock";
  std::string const lock_name{ lock_oss.str() };

  return ensure_entry(*m,
                      entry_dir,
                      m->locks_dir() / lock_name,
                      identity,
                      k,
                      on_lock_contended);
}

cache::ensure_result cache::ensure_spec(
    std::string_view identity,
    std::string_view source_key,
    envy::platform::file_lock::contended_cb_t on_lock_contended) {
  if (!util_is_safe_path_component(identity)) {
    throw std::runtime_error("cache: invalid spec identity: '" + std::string{ identity } +
                             "'");
  }
  auto const digest{ blake3_hash(source_key.data(), source_key.size()) };
  std::string const id{ identity };
  std::string const source_slug{ "blake3-" + util_bytes_to_hex(digest.data(), 8) };
  return ensure_entry(*m,
                      m->specs_dir() / id / source_slug,
                      m->locks_dir() / ("spec." + id + "." + source_slug + ".lock"),
                      identity,
                      id + "-" + source_slug,
                      on_lock_contended);
}

cache::envy_ensure_result cache::ensure_envy(std::string_view version) {
  path const envy_dir{ m->root_ / "envy" / std::string{ version } };
  path const binary_path{ envy_dir / platform::exe_name("envy") };
  path const types_path{ envy_dir / "envy.lua" };

  if (std::filesystem::exists(binary_path) && std::filesystem::exists(types_path)) {
    return { envy_dir, binary_path, types_path, true, std::nullopt };
  }

  std::filesystem::create_directories(m->locks_dir());
  platform::file_lock lock{ m->locks_dir() /
                            ("envy." + std::string{ version } + ".lock") };

  // Re-check after lock (another process may have completed)
  if (std::filesystem::exists(binary_path) && std::filesystem::exists(types_path)) {
    return { envy_dir, binary_path, types_path, true, std::nullopt };
  }

  std::filesystem::create_directories(envy_dir);

  return { envy_dir, binary_path, types_path, false, std::move(lock) };
}

}  // namespace envy
