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

path resolve_cache_root(std::optional<path> const &cli_override,
                        std::optional<std::string> const &manifest_cache,
                        path const &manifest_dir) {
  // Every tier resolves to an absolute path: the cache root prefixes every path envy
  // computes, prints, or hands to a Lua phase, so a relative one would silently name a
  // different tree per working directory.  A CLI flag or ENVY_CACHE_ROOT anchors to the
  // cwd that supplied it; a manifest directive anchors to the manifest's directory, the
  // one location that reads the same from every cwd.
  if (cli_override) { return std::filesystem::absolute(*cli_override); }
  if (manifest_cache) {
    auto expanded{ platform::expand_path(*manifest_cache) };
    if (expanded.is_absolute()) { return expanded; }
    if (manifest_dir.empty()) {
      throw std::runtime_error("cache: relative cache directive '" + *manifest_cache +
                               "' has no manifest directory to anchor to");
    }
    return manifest_dir / expanded;
  }
  if (auto def{ platform::get_default_cache_root() }) { return *def; }

  throw std::runtime_error("cannot determine cache root");
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
