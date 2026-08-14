#pragma once

#include "platform.h"
#include "util.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace envy {

// Resolves to an absolute path or throws.  A relative `manifest_cache` anchors to
// `manifest_dir`, never the cwd; pass an empty `manifest_dir` only when no manifest is in
// hand (then a relative directive is an error, not a cwd-relative guess).
std::filesystem::path resolve_cache_root(
    std::optional<std::filesystem::path> const &cli_override,
    std::optional<std::string> const &manifest_cache,
    std::filesystem::path const &manifest_dir);

class cache : unmovable {
 public:
  using path = std::filesystem::path;

  class scoped_entry_lock : unmovable {
   public:
    using ptr_t = std::unique_ptr<scoped_entry_lock>;

    static ptr_t make(path entry_dir,
                      platform::file_lock lock,
                      path lock_path,
                      std::string pkg_identity,
                      std::chrono::steady_clock::time_point lock_acquired_at);
    ~scoped_entry_lock();

    void mark_install_complete();
    void mark_user_managed();
    void mark_fetch_complete();
    void mark_preserve_fetch();
    bool is_install_complete() const;
    bool is_fetch_complete() const;

    path install_dir() const;
    path stage_dir() const;
    path fetch_dir() const;
    path work_dir() const;
    path tmp_dir() const;

   private:
    scoped_entry_lock(path entry_dir,
                      platform::file_lock lock,
                      path lock_path,
                      std::string pkg_identity,
                      std::chrono::steady_clock::time_point lock_acquired_at);

    struct impl;
    std::unique_ptr<impl> m;
  };

  explicit cache(std::optional<path> root = std::nullopt);
  ~cache();

  path const &root() const;

  struct ensure_result {
    path entry_path;                // entry directory containing metadata and pkg/
    path pkg_path;                  // entry_path / "pkg"
    scoped_entry_lock::ptr_t lock;  // if present, lock held for installation
  };

  ensure_result ensure_pkg(std::string_view identity,
                           std::string_view platform,
                           std::string_view arch,
                           std::string_view hash_prefix);

  path compute_pkg_path(std::string_view identity,
                        std::string_view platform,
                        std::string_view arch,
                        std::string_view hash_prefix) const;

  // `source_key` canonically describes where the spec's bytes come from -- URL and
  // sha256, git URL and ref, local path. It is part of the entry key, so a spec
  // redeclared against a different source lands in a different entry instead of
  // silently reusing the old one: a complete entry is never revalidated, and
  // identity alone does not pin content.
  ensure_result ensure_spec(std::string_view identity, std::string_view source_key);

  struct envy_ensure_result {
    path envy_dir;                            // $CACHE/envy/$VERSION/
    path binary_path;                         // envy_dir / "envy" (or "envy.exe")
    path types_path;                          // envy_dir / "envy.lua"
    bool already_cached;                      // true if binary+types already exist
    std::optional<platform::file_lock> lock;  // held while !already_cached
  };

  // Check/prepare envy version directory in cache.
  // If binary+types already exist, returns already_cached=true.
  // Otherwise acquires lock, creates directories, returns already_cached=false
  // with lock held so caller can deploy.
  envy_ensure_result ensure_envy(std::string_view version);

  static bool is_entry_complete(std::filesystem::path const &entry_dir);

  // Canonical cache key: identity-platform-arch-blake3-hash_prefix
  static std::string key(std::string_view identity,
                         std::string_view platform,
                         std::string_view arch,
                         std::string_view hash_prefix);

 private:
  struct impl;
  std::unique_ptr<impl> m;
};

}  // namespace envy
