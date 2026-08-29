#pragma once

#include "platform.h"
#include "util.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

// Which tree a project's packages live in: its own, or the user-wide one.
enum class cache_mode { LOCAL, SHARED };

// Zero-byte markers under the state dir. Existence is the whole signal -- no contents to
// read, so bash's `[[ -f ]]`, cmd's `if exist` and `fs::exists` cannot disagree the way
// three line-readers would over CRLF, a trailing space, or an empty file.
inline constexpr char kCacheLocalMarker[]{ ".envy-cache-local" };
inline constexpr char kCacheSharedMarker[]{ ".envy-cache-shared" };

// Where a local tree goes when '@envy cache-local' is absent but local mode was asked for
// -- by '@envy cache-mode "local"' or by `envy cache --local`. Also the entry .luarc.json
// lists for a local checkout, so it lives here rather than in one .cpp.
inline constexpr char kDefaultCacheLocal[]{ ".envy/cache" };

// The mode tier, kept pure so it is exhaustively unit-testable without touching the
// filesystem: callers stat the two markers and pass what they found. `declared` is
// '@envy cache-mode', `has_cache_local` whether '@envy cache-local' is present -- naming a
// local tree implies wanting it, or the directive would be inert. Throws when both markers
// exist, a state envy never writes.
cache_mode resolve_cache_mode(bool local_marker,
                              bool shared_marker,
                              std::optional<cache_mode> declared,
                              bool has_cache_local);

// Accepts one or more non-empty path components, none of them '.' or '..', with no drive
// letter, leading separator, '~', '$' or '%'. Returns the reason it was rejected, or
// nullopt when valid.  Guards '@envy cache-local' and '@envy state-dir': both name a
// subdirectory of the project, so anything else is either an escape or a stale absolute
// path from the removed cache-posix/cache-win directives.
std::optional<std::string> validate_project_relative_path(std::string_view value);

// Everything the cache root depends on, gathered at one call site so a process resolves
// once and threads the answer through.  Resolving per-consumer let a concurrent
// `envy cache --local` land between two resolutions and send one process looking for its
// binary in one tree while it installed packages into another.
struct cache_root_request {
  std::optional<std::filesystem::path> cli_override;  // --cache-root / ENVY_CACHE_ROOT
  std::optional<std::string> cache_local;             // @envy cache-local
  std::optional<cache_mode> declared_mode;            // @envy cache-mode
  std::optional<std::string> state_dir;               // @envy state-dir
  std::filesystem::path manifest_dir;                 // empty when no manifest was found
};

// Names the tier that decided, for `envy cache` and the first-run notice.
enum class cache_root_tier { CLI_OVERRIDE, MARKER, DIRECTIVE, IMPLIED_LOCAL, DEFAULT };

struct cache_root_resolution {
  std::filesystem::path root;
  cache_mode mode{ cache_mode::SHARED };
  cache_root_tier tier{ cache_root_tier::DEFAULT };
};

// Resolves to an absolute, lexically-normal path or throws.  A `cache-local` tree anchors
// to `manifest_dir`, never the cwd, so one manifest names one tree from every working
// directory.  Normalization is not cosmetic: `operator/` leaves `C:\proj` / `out/.envy` as
// `C:\proj\out/.envy`, which no launcher would ever print.
cache_root_resolution resolve_cache_root(cache_root_request const &req);

// Where this project's cache would be in `mode`, with the tiers that *decide* the mode
// skipped.  `envy cache --local/--shared` self-deploys before its own marker is written,
// so it has to name the tree it is about to establish: deploying into the current one
// drops a binary in the tree the user is abandoning.
//
// An override still wins, and still reports SHARED/CLI_OVERRIDE rather than `mode` -- it
// names one tree outright, and that tree is the user's own however this project resolves.
// Returning the whole resolution rather than a path is what keeps that honest: a caller
// handed only a path would pair it with the mode it asked for, and a LOCAL-mode override
// root would then be skipped by everything keyed on the mode, shell hooks included.
cache_root_resolution cache_root_for_mode(cache_root_request const &req, cache_mode mode);

// The user's own cache root -- the override, else the platform default; nullopt when
// neither is determinable (no HOME/XDG_CACHE_HOME/LOCALAPPDATA).  A project on a local
// cache may *read* this tree for an envy binary it can run, but nothing populates it on
// that project's behalf; see envy_binary_candidates.
std::optional<std::filesystem::path> resolve_user_wide_cache_root(
    std::optional<std::filesystem::path> const &cli_override);

// Paths to try, in order, when looking for `version`'s envy binary.  Pure: the caller
// stats them and takes the first that will actually run, exactly as resolve_cache_mode has
// its caller stat the two markers -- a signature that stats internally could not be unit
// tested without touching the filesystem.
//
// The second candidate exists only for a LOCAL tree, and only with no '@envy sha256sums'.
// Reading the user's own tree saves re-downloading a binary they already have; a *pinned*
// project running bytes it never attested does not, because the cache fast path never
// re-hashes and that tree is written by every other project on the box.  A SHARED tree
// never looks in a project-local one: a hostile clone shipping its own envy/<ver>/envy
// would be arbitrary code execution on the first launcher run.
std::vector<std::filesystem::path> envy_binary_candidates(
    cache_root_resolution const &resolved,
    std::optional<std::filesystem::path> const &user_wide_root,
    std::string_view version,
    bool has_sums_pin);

// Absolute path of the state dir holding the override markers, or nullopt when no
// manifest is in hand.  Defaults to the manifest's own directory rather than to `.envy`:
// the default local tree is `.envy/cache`, so a `.envy` state dir would put a user's "use
// the shared cache" marker *inside* the tree they just opted out of, and `rm -rf .envy`
// would silently revert them and refetch everything.
std::optional<std::filesystem::path> resolve_state_dir(
    std::optional<std::string> const &state_dir,
    std::filesystem::path const &manifest_dir);

// Human-readable tier name for `envy cache`'s report line.
char const *cache_root_tier_name(cache_root_tier tier);

// Says where packages are about to land, once, while the resolved tree still holds none --
// so a user learns before gigabytes arrive rather than after. Goes to stderr, keeping
// `envy cache --root` machine-readable, and is never a prompt: a prompt would hang CI and
// non-TTY stdin. No state records that it was shown; the absence of the cache's packages/
// directory is the trigger, which makes it self-limiting and correctly brings it back
// after a teardown or a mode switch. `bin_dir` is '@envy bin', so the suggested command
// matches the project's own layout.
void cache_announce_root_once(cache_root_resolution const &resolved,
                              std::optional<std::string> const &bin_dir);

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

  // `on_lock_contended` fires when another envy holds this entry and this call is about to
  // block on it for however long that takes. See tui_actions::lock_wait_spinner.
  ensure_result ensure_pkg(std::string_view identity,
                           std::string_view platform,
                           std::string_view arch,
                           std::string_view hash_prefix,
                           platform::file_lock::contended_cb_t on_lock_contended = {});

  path compute_pkg_path(std::string_view identity,
                        std::string_view platform,
                        std::string_view arch,
                        std::string_view hash_prefix) const;

  // `source_key` canonically describes where the spec's bytes come from -- URL and
  // sha256, git URL and ref, local path. It is part of the entry key, so a spec
  // redeclared against a different source lands in a different entry instead of
  // silently reusing the old one: a complete entry is never revalidated, and
  // identity alone does not pin content.
  ensure_result ensure_spec(std::string_view identity,
                            std::string_view source_key,
                            platform::file_lock::contended_cb_t on_lock_contended = {});

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
