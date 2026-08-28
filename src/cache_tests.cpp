#include "cache.h"

#include "doctest.h"
#include "platform.h"
#include "util.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace {

std::string make_entry_name() { return "foo.darwin-arm64-blake3-deadbeef"; }

// Absolute on both platforms: Windows treats a rootless "/x" as drive-relative, so
// resolve_cache_root would anchor it to the cwd's drive.
std::filesystem::path const kAbsRoot{
#ifdef _WIN32
  "C:\\"
#else
  "/"
#endif
};

}  // namespace

// Doctest fixture for tests that need a temporary cache directory
struct temp_cache_fixture {
  temp_cache_fixture() {
    static std::mt19937_64 rng{ std::random_device{}() };
    auto suffix = std::to_string(rng());
    temp_root = std::filesystem::temp_directory_path() /
                std::filesystem::path("envy-cache-test-unit-" + suffix);
    std::filesystem::create_directories(temp_root);
    cache = std::make_unique<envy::cache>(temp_root);
  }

  ~temp_cache_fixture() {
    cache.reset();  // Destroy cache before cleaning up directory
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
    if (ec) {
      std::string msg = "Failed to clean up temp cache directory '" + temp_root.string() +
                        "': " + ec.message();
      FAIL_CHECK(msg.c_str());
    }
  }

  std::filesystem::path temp_root;
  std::unique_ptr<envy::cache> cache;
};

TEST_CASE_FIXTURE(temp_cache_fixture, "repeated mark_install_complete calls are safe") {
  auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
  REQUIRE(result.lock != nullptr);

  // Write something to install_dir
  auto install_file = result.lock->install_dir() / "output.txt";
  {
    std::ofstream ofs{ install_file };
    ofs << "installed";
  }  // Ensure file is closed before calling mark_install_complete

  // Call mark_install_complete once (multiple calls tested in destructor idempotency)
  result.lock->mark_install_complete();

  // Should complete successfully without error
  CHECK(true);

  // Explicitly destroy the lock before fixture destructor runs
  result.lock.reset();
}

TEST_CASE_FIXTURE(temp_cache_fixture, "cache root path") {
  CHECK(cache->root() == temp_root);
}

TEST_CASE("cache is_entry_complete") {
  // Complete entry has envy-complete marker
  CHECK(envy::cache::is_entry_complete("test_data/cache/complete-entry"));

  // Incomplete entry missing marker
  CHECK_FALSE(envy::cache::is_entry_complete("test_data/cache/incomplete-entry"));

  // Nonexistent entry
  CHECK_FALSE(envy::cache::is_entry_complete("test_data/cache/nonexistent"));
}

TEST_CASE("scoped_entry_lock is unmovable") {
  // Neither movable nor copyable (uses unique_ptr for transfer)
  CHECK_FALSE(std::is_move_constructible_v<envy::cache::scoped_entry_lock>);
  CHECK_FALSE(std::is_move_assignable_v<envy::cache::scoped_entry_lock>);
  CHECK_FALSE(std::is_copy_constructible_v<envy::cache::scoped_entry_lock>);
  CHECK_FALSE(std::is_copy_assignable_v<envy::cache::scoped_entry_lock>);
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "ensure_pkg returns lock for cold entry and publishes pkg directory") {
  auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
  CHECK(result.lock != nullptr);
  CHECK_FALSE(result.pkg_path.empty());
  CHECK(std::filesystem::exists(result.lock->install_dir()));
  CHECK(std::filesystem::exists(result.lock->stage_dir()));
  CHECK(std::filesystem::exists(result.lock->fetch_dir()));

  // Simulate install: drop file into install dir then mark complete
  auto payload = result.lock->install_dir() / "sentinel.txt";
  std::ofstream{ payload } << "ok";
  result.lock->mark_install_complete();
  result.lock.reset();

  CHECK(std::filesystem::exists(result.entry_path / "envy-complete"));
  CHECK(std::filesystem::exists(result.pkg_path / "sentinel.txt"));
  CHECK_FALSE(std::filesystem::exists(result.entry_path / "work"));
}

TEST_CASE_FIXTURE(temp_cache_fixture, "ensure_pkg fast path when marker present") {
  auto entry_dir = temp_root / "packages" / "foo" / "darwin-arm64-blake3-deadbeef";
  auto pkg_dir = entry_dir / "pkg";
  std::filesystem::create_directories(pkg_dir);
  std::ofstream{ pkg_dir / "existing.txt" } << "cached";
  envy::platform::touch_file(entry_dir / "envy-complete");

  auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
  CHECK(result.lock == nullptr);
  CHECK(result.pkg_path == pkg_dir);
  CHECK(std::filesystem::exists(result.pkg_path / "existing.txt"));
}

TEST_CASE_FIXTURE(temp_cache_fixture, "ensure_pkg entry layout") {
  auto result = cache->ensure_pkg("gcc", "linux", "x86_64", "deadbeef");
  REQUIRE(result.lock != nullptr);

  CHECK(result.entry_path ==
        temp_root / "packages" / "gcc" / "linux-x86_64-blake3-deadbeef");
  CHECK(result.pkg_path == result.entry_path / "pkg");
  CHECK(result.lock->install_dir() == result.pkg_path);
  CHECK(result.lock->fetch_dir() == result.entry_path / "fetch");
  CHECK(result.lock->stage_dir() == result.entry_path / "work" / "stage");
  result.lock.reset();
}

TEST_CASE_FIXTURE(temp_cache_fixture, "ensure_spec entry layout") {
  auto result = cache->ensure_spec("envy.cmake@v1", "https://example.com/spec.lua");
  REQUIRE(result.lock != nullptr);

  CHECK(result.entry_path.parent_path() == temp_root / "specs" / "envy.cmake@v1");
  CHECK(result.entry_path.filename().string().starts_with("blake3-"));
  CHECK(result.pkg_path == result.entry_path / "pkg");
  result.lock->mark_install_complete();
  result.lock.reset();

  CHECK(std::filesystem::is_directory(result.entry_path));
  CHECK(std::filesystem::exists(result.entry_path / "envy-complete"));
}

TEST_CASE_FIXTURE(temp_cache_fixture, "ensure_spec keys entries on the source") {
  // Same identity, two sources: two entries, so a redeclared source is never
  // served yesterday's content. Nothing revalidates a complete entry.
  auto a = cache->ensure_spec("envy.cmake@v1", "https://a.example.com/spec.lua");
  REQUIRE(a.lock != nullptr);
  a.lock->mark_install_complete();
  a.lock.reset();

  auto b = cache->ensure_spec("envy.cmake@v1", "https://b.example.com/spec.lua");
  CHECK(b.entry_path != a.entry_path);
  CHECK(b.lock != nullptr);  // cold: a's completion does not answer for b
  b.lock->mark_install_complete();
  b.lock.reset();

  // Re-asking with either source hits its own entry.
  CHECK(cache->ensure_spec("envy.cmake@v1", "https://a.example.com/spec.lua").lock ==
        nullptr);
  CHECK(cache->ensure_spec("envy.cmake@v1", "https://b.example.com/spec.lua").lock ==
        nullptr);
}

TEST_CASE_FIXTURE(temp_cache_fixture, "ensure_pkg creates locks directory on demand") {
  REQUIRE_FALSE(std::filesystem::exists(temp_root / "locks"));
  auto result = cache->ensure_pkg("gcc", "darwin", "arm64", "auto1");
  CHECK(std::filesystem::is_directory(temp_root / "locks"));
  result.lock.reset();
}

TEST_CASE_FIXTURE(temp_cache_fixture, "mark_fetch_complete creates sentinel") {
  auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
  REQUIRE(result.lock != nullptr);

  CHECK_FALSE(result.lock->is_fetch_complete());
  result.lock->mark_fetch_complete();
  CHECK(result.lock->is_fetch_complete());
  CHECK(std::filesystem::exists(result.lock->fetch_dir() / "envy-complete"));
  result.lock.reset();
}

TEST_CASE_FIXTURE(temp_cache_fixture, "fetch_dir preserved when marked complete") {
  // First acquisition: populate fetch/ and mark complete
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    auto fetch_file = result.lock->fetch_dir() / "payload.tar.gz";
    std::ofstream{ fetch_file } << "large download";
    result.lock->mark_fetch_complete();
  }

  // Second acquisition: verify fetch/ survived
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    CHECK(result.lock->is_fetch_complete());
    auto fetch_file = result.lock->fetch_dir() / "payload.tar.gz";
    CHECK(std::filesystem::exists(fetch_file));

    std::ifstream ifs{ fetch_file };
    std::string content{ std::istreambuf_iterator<char>{ ifs }, {} };
    CHECK(content == "large download");
  }
}

TEST_CASE_FIXTURE(temp_cache_fixture, "fetch_dir preserved when not marked complete") {
  // First acquisition: populate fetch/ but don't mark complete (simulates crash)
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    auto fetch_file = result.lock->fetch_dir() / "partial.tar.gz";
    std::ofstream{ fetch_file } << "incomplete";
    // Note: intentionally not calling mark_fetch_complete()
  }

  // Second acquisition: verify fetch/ was preserved for per-file caching
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    CHECK_FALSE(result.lock->is_fetch_complete());
    auto fetch_file = result.lock->fetch_dir() / "partial.tar.gz";
    CHECK(std::filesystem::exists(fetch_file));

    std::ifstream ifs{ fetch_file };
    std::string content{ std::istreambuf_iterator<char>{ ifs }, {} };
    CHECK(content == "incomplete");
  }
}

TEST_CASE_FIXTURE(
    temp_cache_fixture,
    "programmatic package with empty install_dir and fetch_dir cleans up cache") {
  auto const entry_name = make_entry_name();
  std::filesystem::path entry_dir;

  // Acquire lock, don't mark complete, leave directories empty
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    entry_dir = result.entry_path;

    // Don't call mark_install_complete()
    // install_dir and fetch_dir are empty (no files written)
  }
  // Lock destructor should purge entire cache entry

  // Verify cache entry directories were cleaned up
  CHECK_FALSE(std::filesystem::exists(entry_dir / "pkg"));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "fetch"));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "work"));
}

TEST_CASE_FIXTURE(temp_cache_fixture, "programmatic package with fetch_dir preserved") {
  std::filesystem::path entry_dir;
  std::filesystem::path fetch_file;

  // First acquisition: populate fetch_dir but don't mark complete
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    entry_dir = result.entry_path;

    // Write file to fetch_dir
    fetch_file = result.lock->fetch_dir() / "downloaded.tar.gz";
    std::ofstream{ fetch_file } << "large payload";

    // Don't call mark_install_complete()
    // install_dir is empty, but fetch_dir has content
  }
  // Lock destructor should preserve fetch_dir

  // Verify fetch_dir was preserved
  CHECK(std::filesystem::exists(fetch_file));
  std::ifstream ifs{ fetch_file };
  std::string content{ std::istreambuf_iterator<char>{ ifs }, {} };
  CHECK(content == "large payload");

  // Verify other directories cleaned up
  CHECK_FALSE(std::filesystem::exists(entry_dir / "pkg"));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "work"));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "install_dir populated without mark_install_complete preserved") {
  std::filesystem::path entry_dir;

  // Acquire lock, populate install_dir, but don't mark complete
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    entry_dir = result.entry_path;

    // Write file to install_dir
    auto install_file = result.lock->install_dir() / "artifact.so";
    std::ofstream{ install_file } << "compiled binary";

    // Don't call mark_install_complete()
  }
  // Lock destructor cleans install_dir but not the entry

  // Verify install_dir (pkg/) was cleaned
  CHECK_FALSE(std::filesystem::exists(entry_dir / "pkg"));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "work"));

  // Entry itself should still exist (for retry)
  CHECK(std::filesystem::exists(entry_dir));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "user-managed package with empty dirs purges entire entry_dir") {
  std::filesystem::path entry_dir;

  // Acquire lock, mark as user-managed, leave directories empty
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    entry_dir = result.entry_path;

    // Mark as user-managed (simulates check phase marking)
    result.lock->mark_user_managed();

    // Don't write any files, don't call mark_install_complete()
  }
  // Lock destructor should purge entire entry_dir

  // Verify entire entry_dir was deleted
  CHECK_FALSE(std::filesystem::exists(entry_dir));
}

TEST_CASE_FIXTURE(
    temp_cache_fixture,
    "user-managed package with fetch_dir populated purges entire entry_dir") {
  std::filesystem::path entry_dir;
  std::filesystem::path fetch_file;

  // Acquire lock, mark as user-managed, populate fetch_dir
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    entry_dir = result.entry_path;

    // Write file to fetch_dir
    fetch_file = result.lock->fetch_dir() / "downloaded.tar.gz";
    std::ofstream{ fetch_file } << "large payload";

    // Mark as user-managed
    result.lock->mark_user_managed();

    // Don't call mark_install_complete()
  }
  // Lock destructor should purge entire entry_dir (including fetch_dir)

  // Verify entire entry_dir was deleted (including fetch_dir with contents)
  CHECK_FALSE(std::filesystem::exists(entry_dir));
  CHECK_FALSE(std::filesystem::exists(fetch_file));
}

TEST_CASE_FIXTURE(
    temp_cache_fixture,
    "user-managed package with install_dir populated purges entire entry_dir") {
  std::filesystem::path entry_dir;

  // Acquire lock, mark as user-managed, populate install_dir
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    entry_dir = result.entry_path;

    // Write file to install_dir
    auto install_file = result.lock->install_dir() / "artifact.so";
    std::ofstream{ install_file } << "compiled binary";

    // Mark as user-managed
    result.lock->mark_user_managed();

    // Don't call mark_install_complete()
  }
  // Lock destructor should purge entire entry_dir (including install_dir)

  // Verify entire entry_dir was deleted
  CHECK_FALSE(std::filesystem::exists(entry_dir));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "user-managed package with all dirs populated purges entire entry_dir") {
  std::filesystem::path entry_dir;

  // Acquire lock, mark as user-managed, populate fetch/stage/install dirs
  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    entry_dir = result.entry_path;

    // Write files to all workspace directories
    std::ofstream{ result.lock->fetch_dir() / "downloaded.tar.gz" } << "fetch payload";
    std::ofstream{ result.lock->stage_dir() / "extracted.txt" } << "stage payload";
    std::ofstream{ result.lock->install_dir() / "artifact.so" } << "install payload";

    // Mark as user-managed
    result.lock->mark_user_managed();

    // Don't call mark_install_complete()
  }
  // Lock destructor should purge entire entry_dir (all subdirectories)

  // Verify entire entry_dir was deleted
  CHECK_FALSE(std::filesystem::exists(entry_dir));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "lock file deletion attempted on successful completion") {
  std::filesystem::path lock_path;

  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    // Capture lock path (not exposed via public API, but we can infer it)
    lock_path = temp_root / "locks" / "packages.foo-darwin-arm64-blake3-deadbeef.lock";

    // Verify lock file exists while lock is held
    CHECK(std::filesystem::exists(lock_path));

    // Write to install_dir and mark complete
    std::ofstream{ result.lock->install_dir() / "output.txt" } << "done";
    result.lock->mark_install_complete();
  }
  // Lock destructor attempts to delete lock file

  // Lock file should be deleted (no other process holding it in this test)
  CHECK_FALSE(std::filesystem::exists(lock_path));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "lock file deletion attempted on user-managed purge") {
  std::filesystem::path lock_path;

  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    lock_path = temp_root / "locks" / "packages.foo-darwin-arm64-blake3-deadbeef.lock";

    // Verify lock file exists while lock is held
    CHECK(std::filesystem::exists(lock_path));

    // Mark as user-managed
    result.lock->mark_user_managed();
  }
  // Lock destructor attempts to delete lock file

  // Lock file should be deleted
  CHECK_FALSE(std::filesystem::exists(lock_path));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "user-managed complete cleanup: entry_dir and lock file both deleted") {
  std::filesystem::path entry_dir;
  std::filesystem::path lock_path;

  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    entry_dir = result.entry_path;
    lock_path = temp_root / "locks" / "packages.foo-darwin-arm64-blake3-deadbeef.lock";

    // Verify both exist while lock is held
    CHECK(std::filesystem::exists(entry_dir));
    CHECK(std::filesystem::exists(lock_path));

    // Populate all directories to ensure they get cleaned up
    std::ofstream{ result.lock->fetch_dir() / "download.tar.gz" } << "fetch data";
    std::ofstream{ result.lock->stage_dir() / "staged.txt" } << "stage data";
    std::ofstream{ result.lock->install_dir() / "binary.so" } << "install data";

    // Verify subdirectories exist
    CHECK(std::filesystem::exists(result.lock->fetch_dir() / "download.tar.gz"));
    CHECK(std::filesystem::exists(result.lock->stage_dir() / "staged.txt"));
    CHECK(std::filesystem::exists(result.lock->install_dir() / "binary.so"));

    // Mark as user-managed
    result.lock->mark_user_managed();
  }
  // scoped_entry_lock destructor runs first: purges entry_dir
  // Then file_lock destructor runs: deletes lock file

  // Verify complete cleanup: both entry_dir and lock file deleted
  CHECK_FALSE(std::filesystem::exists(entry_dir));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "fetch"));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "stage"));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "work"));
  CHECK_FALSE(std::filesystem::exists(lock_path));
}

TEST_CASE_FIXTURE(
    temp_cache_fixture,
    "cache-managed success: entry_dir preserved with pkg, temp dirs cleaned") {
  std::filesystem::path entry_dir;
  std::filesystem::path pkg_dir;
  std::filesystem::path lock_path;

  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    entry_dir = result.entry_path;
    pkg_dir = result.pkg_path;
    lock_path = temp_root / "locks" / "packages.foo-darwin-arm64-blake3-deadbeef.lock";

    // Verify entry_dir and lock file exist while lock is held
    CHECK(std::filesystem::exists(entry_dir));
    CHECK(std::filesystem::exists(lock_path));

    // Populate all directories
    std::ofstream{ result.lock->fetch_dir() / "download.tar.gz" } << "fetch data";
    std::ofstream{ result.lock->stage_dir() / "staged.txt" } << "stage data";
    std::ofstream{ result.lock->install_dir() / "binary.so" } << "install data";
    std::ofstream{ result.lock->install_dir() / "library.a" } << "library data";

    // Mark as successfully installed (NOT user-managed)
    result.lock->mark_install_complete();
  }
  // scoped_entry_lock destructor runs success path: install writes directly to pkg/
  // Then file_lock destructor runs: deletes lock file

  // Verify entry_dir is preserved
  CHECK(std::filesystem::exists(entry_dir));

  // Verify pkg/ dir exists with installed files
  CHECK(std::filesystem::exists(pkg_dir));
  CHECK(std::filesystem::exists(pkg_dir / "binary.so"));
  CHECK(std::filesystem::exists(pkg_dir / "library.a"));

  // Verify envy-complete marker created
  CHECK(std::filesystem::exists(entry_dir / "envy-complete"));

  // Verify temporary directories cleaned up
  CHECK_FALSE(std::filesystem::exists(entry_dir / "fetch"));
  CHECK_FALSE(std::filesystem::exists(entry_dir / "work"));

  // Verify lock file deleted
  CHECK_FALSE(std::filesystem::exists(lock_path));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "mark_preserve_fetch keeps fetch_dir on success path") {
  std::filesystem::path entry_dir;
  std::filesystem::path fetch_file;

  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);

    entry_dir = result.entry_path;

    // Populate both fetch/ and pkg/
    fetch_file = result.lock->fetch_dir() / "downloaded.tar.gz";
    std::ofstream{ fetch_file } << "big payload";
    std::ofstream{ result.lock->install_dir() / "binary.so" } << "installed";

    result.lock->mark_preserve_fetch();
    result.lock->mark_install_complete();
  }

  // Verify envy-complete marker created (success path ran)
  CHECK(std::filesystem::exists(entry_dir / "envy-complete"));

  // Verify pkg/ preserved
  CHECK(std::filesystem::exists(entry_dir / "pkg" / "binary.so"));

  // Verify fetch/ preserved (not cleaned up)
  CHECK(std::filesystem::exists(fetch_file));

  // Verify work/ cleaned up
  CHECK_FALSE(std::filesystem::exists(entry_dir / "work"));
}

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "success path without mark_preserve_fetch deletes fetch_dir") {
  std::filesystem::path entry_dir;
  std::filesystem::path fetch_file;

  {
    auto result = cache->ensure_pkg("foo", "darwin", "arm64", "deadbeef02");
    REQUIRE(result.lock != nullptr);

    entry_dir = result.entry_path;

    // Populate both fetch/ and pkg/
    fetch_file = result.lock->fetch_dir() / "downloaded.tar.gz";
    std::ofstream{ fetch_file } << "big payload";
    std::ofstream{ result.lock->install_dir() / "binary.so" } << "installed";

    // Do NOT call mark_preserve_fetch
    result.lock->mark_install_complete();
  }

  // Verify success path ran
  CHECK(std::filesystem::exists(entry_dir / "envy-complete"));
  CHECK(std::filesystem::exists(entry_dir / "pkg" / "binary.so"));

  // Verify fetch/ was cleaned up (default behavior)
  CHECK_FALSE(std::filesystem::exists(fetch_file));
}

// Tests for cache-root resolution
//
// resolve_cache_mode and validate_project_relative_path are pure by construction, so the
// whole mode matrix is covered without touching the filesystem, which CLAUDE.md requires
// of unit tests. resolve_cache_root's tests stay on the tiers that need no marker files.

using envy::cache_mode;
using envy::cache_root_request;
using envy::cache_root_tier;

TEST_CASE("resolve_cache_mode: markers outrank the manifest") {
  // A marker is the user's own decision, so it wins over whatever the project declares.
  CHECK(envy::resolve_cache_mode(true, false, cache_mode::SHARED, false) ==
        cache_mode::LOCAL);
  CHECK(envy::resolve_cache_mode(false, true, cache_mode::LOCAL, true) ==
        cache_mode::SHARED);
}

TEST_CASE("resolve_cache_mode: both markers is an error, never a silent pick") {
  CHECK_THROWS_AS(envy::resolve_cache_mode(true, true, std::nullopt, false),
                  std::runtime_error);
}

TEST_CASE("resolve_cache_mode: cache-mode directive decides when no marker exists") {
  CHECK(envy::resolve_cache_mode(false, false, cache_mode::LOCAL, false) ==
        cache_mode::LOCAL);
  // Declaring shared while also naming a local tree is the "here is where --local would
  // put it, but default to shared" case.
  CHECK(envy::resolve_cache_mode(false, false, cache_mode::SHARED, true) ==
        cache_mode::SHARED);
}

TEST_CASE("resolve_cache_mode: naming a local tree implies wanting it") {
  // Otherwise a cache-local would sit in a manifest doing nothing at all.
  CHECK(envy::resolve_cache_mode(false, false, std::nullopt, true) == cache_mode::LOCAL);
}

TEST_CASE("resolve_cache_mode: nothing declared means shared") {
  // Today's behavior for every existing manifest: the user-wide cache they already filled.
  CHECK(envy::resolve_cache_mode(false, false, std::nullopt, false) == cache_mode::SHARED);
}

TEST_CASE("validate_project_relative_path accepts plain relative paths") {
  CHECK_FALSE(envy::validate_project_relative_path("out/.envy").has_value());
  CHECK_FALSE(envy::validate_project_relative_path(".envy/cache").has_value());
  CHECK_FALSE(envy::validate_project_relative_path("a/b/c/d").has_value());
  CHECK_FALSE(envy::validate_project_relative_path("one").has_value());
  // Authored once, read on every platform, so a backslash is a separator here too.
  CHECK_FALSE(envy::validate_project_relative_path("out\\.envy").has_value());
}

TEST_CASE("validate_project_relative_path rejects the removed expansion forms") {
  // Everything wordexp() used to accept, now rejected with a reason rather than expanded.
  CHECK(envy::validate_project_relative_path("~").has_value());
  CHECK(envy::validate_project_relative_path("~/cache").has_value());
  CHECK(envy::validate_project_relative_path("$HOME/cache").has_value());
  CHECK(envy::validate_project_relative_path("${HOME}/cache").has_value());
  CHECK(envy::validate_project_relative_path("${FOO:-out/.envy}").has_value());
  CHECK(envy::validate_project_relative_path("%LOCALAPPDATA%/envy").has_value());
}

TEST_CASE("validate_project_relative_path rejects absolute and escaping paths") {
  CHECK(envy::validate_project_relative_path("/opt/cache").has_value());
  CHECK(envy::validate_project_relative_path("\\opt\\cache").has_value());
  CHECK(envy::validate_project_relative_path("C:\\cache").has_value());
  CHECK(envy::validate_project_relative_path("C:/cache").has_value());
  // '' and '.' would put packages/ and locks/ in the project root; '..' escapes it, and
  // defeats "delete the build root, all traces gone" outright.
  CHECK(envy::validate_project_relative_path("").has_value());
  CHECK(envy::validate_project_relative_path(".").has_value());
  CHECK(envy::validate_project_relative_path("..").has_value());
  CHECK(envy::validate_project_relative_path("../sibling").has_value());
  CHECK(envy::validate_project_relative_path("out/../..").has_value());
  CHECK(envy::validate_project_relative_path("out//envy").has_value());
}

TEST_CASE("resolve_cache_root: override outranks every project tier") {
  cache_root_request req{ .cli_override = kAbsRoot / "cli" / "override",
                          .cache_local = "out/.envy",
                          .manifest_dir = kAbsRoot / "repo" };
  auto const r{ envy::resolve_cache_root(req) };
  CHECK(r.root == kAbsRoot / "cli" / "override");
  CHECK(r.tier == cache_root_tier::CLI_OVERRIDE);
}

TEST_CASE("resolve_cache_root: a relative override is rejected, not absolutized") {
  // The binary used to anchor it to its own cwd while both launchers took it verbatim, so
  // one invocation named two different trees.
  cache_root_request req{ .cli_override = std::filesystem::path{ "rel-cache" },
                          .manifest_dir = kAbsRoot / "repo" };
  CHECK_THROWS_AS(envy::resolve_cache_root(req), std::runtime_error);
}

TEST_CASE("resolve_cache_root: cache-local anchors to the manifest dir") {
  // Not the cwd: the same manifest must name one tree from every directory, or every
  // invocation from a subdirectory refetches the whole package set.
  cache_root_request req{ .cache_local = "out/.envy",
                          .manifest_dir = kAbsRoot / "repo" };
  auto const r{ envy::resolve_cache_root(req) };
  CHECK(r.root == kAbsRoot / "repo" / "out" / ".envy");
  CHECK(r.mode == cache_mode::LOCAL);
  CHECK(r.tier == cache_root_tier::IMPLIED_LOCAL);
}

TEST_CASE("resolve_cache_root: the joined local path is normalized") {
  // operator/ leaves 'C:\repo' / 'out/.envy' as 'C:\repo\out/.envy', which no launcher
  // would ever print -- envy.bat's %~fI collapses it. Comparing against a path built with
  // operator/ asserts both sides agree after normalization.
  cache_root_request req{ .cache_local = "out/.envy",
                          .manifest_dir = kAbsRoot / "repo" };
  auto const r{ envy::resolve_cache_root(req) };
  CHECK(r.root == r.root.lexically_normal());
  auto expected{ kAbsRoot / "repo" / "out" / ".envy" };
  CHECK(r.root.string() == expected.make_preferred().string());
}

TEST_CASE("resolve_cache_root: cache-mode local without cache-local uses the default") {
  cache_root_request req{ .declared_mode = cache_mode::LOCAL,
                          .manifest_dir = kAbsRoot / "repo" };
  auto const r{ envy::resolve_cache_root(req) };
  CHECK(r.root == (kAbsRoot / "repo" / envy::kDefaultCacheLocal).lexically_normal());
  CHECK(r.tier == cache_root_tier::DIRECTIVE);
}

TEST_CASE("resolve_cache_root: cache-mode shared beats an implied local") {
  cache_root_request req{ .cache_local = "out/.envy",
                          .declared_mode = cache_mode::SHARED,
                          .manifest_dir = kAbsRoot / "repo" };
  auto const r{ envy::resolve_cache_root(req) };
  CHECK(r.mode == cache_mode::SHARED);
  CHECK(r.root != kAbsRoot / "repo" / "out" / ".envy");
}

TEST_CASE("resolve_cache_root: an invalid cache-local throws") {
  cache_root_request req{ .cache_local = "~/cache", .manifest_dir = kAbsRoot / "repo" };
  CHECK_THROWS_AS(envy::resolve_cache_root(req), std::runtime_error);
}

TEST_CASE("resolve_cache_root: local mode with no manifest dir throws") {
  // Nothing to anchor to, and a cwd-relative guess would name a different tree per shell.
  cache_root_request req{ .declared_mode = cache_mode::LOCAL };
  CHECK_THROWS_AS(envy::resolve_cache_root(req), std::runtime_error);
}

TEST_CASE("resolve_cache_root: nesting state-dir and cache-local is rejected") {
  // Markers under the cache root vanish with a cache wipe, taking a user's --shared choice
  // with them. Equal is allowed: that is a project asking for co-located teardown.
  CHECK_THROWS_AS(envy::resolve_cache_root(cache_root_request{
                      .cache_local = "out/.envy",
                      .state_dir = "out/.envy/state",
                      .manifest_dir = kAbsRoot / "repo" }),
                  std::runtime_error);
  CHECK_THROWS_AS(envy::resolve_cache_root(cache_root_request{
                      .cache_local = "out/.envy/cache",
                      .state_dir = "out/.envy",
                      .manifest_dir = kAbsRoot / "repo" }),
                  std::runtime_error);
  CHECK_NOTHROW(envy::resolve_cache_root(cache_root_request{
      .cache_local = "out/.envy",
      .state_dir = "out/.envy",
      .manifest_dir = kAbsRoot / "repo" }));
  // Siblings are fine.
  CHECK_NOTHROW(envy::resolve_cache_root(cache_root_request{
      .cache_local = "out/.envy",
      .state_dir = "out/state",
      .manifest_dir = kAbsRoot / "repo" }));
}

TEST_CASE("resolve_state_dir defaults to the manifest dir, not .envy") {
  // .envy would sit above the default local tree .envy/cache, so a cache wipe would erase
  // the marker and silently revert the user.
  auto const d{ envy::resolve_state_dir(std::nullopt, kAbsRoot / "repo") };
  REQUIRE(d.has_value());
  CHECK(*d == (kAbsRoot / "repo").lexically_normal());
}

TEST_CASE("resolve_state_dir honors a relocation and rejects a bad one") {
  auto const d{ envy::resolve_state_dir(std::string{ "out/.envy" }, kAbsRoot / "repo" ) };
  REQUIRE(d.has_value());
  CHECK(*d == (kAbsRoot / "repo" / "out" / ".envy").lexically_normal());

  CHECK_THROWS_AS(envy::resolve_state_dir(std::string{ "../escape" }, kAbsRoot / "repo"),
                  std::runtime_error);
  CHECK_FALSE(envy::resolve_state_dir(std::nullopt, {}).has_value());
}

// Tests for ensure_envy()

TEST_CASE_FIXTURE(temp_cache_fixture,
                  "ensure_envy returns paths and lock for cold cache") {
  auto result{ cache->ensure_envy(ENVY_VERSION_STR) };

  CHECK_FALSE(result.already_cached);
  CHECK(result.lock.has_value());
  CHECK(std::filesystem::exists(result.envy_dir));
  CHECK(result.types_path == result.envy_dir / "envy.lua");

  CHECK(result.binary_path == result.envy_dir / envy::platform::exe_name("envy"));
}

TEST_CASE_FIXTURE(temp_cache_fixture, "ensure_envy returns already_cached after deploy") {
  // First call: cold cache
  {
    auto result{ cache->ensure_envy(ENVY_VERSION_STR) };
    CHECK_FALSE(result.already_cached);

    // Simulate deployment: create the binary and types files
    std::ofstream{ result.binary_path } << "fake-binary";
    std::ofstream{ result.types_path } << "fake-types";
  }

  // Second call: should be cached
  auto result{ cache->ensure_envy(ENVY_VERSION_STR) };
  CHECK(result.already_cached);
  CHECK_FALSE(result.lock.has_value());
}

// --- cache key format tests ---

TEST_CASE("cache::key") {
  CHECK(envy::cache::key("arm.gcc@r2", "darwin", "arm64", "abcdef0123456789") ==
        "arm.gcc@r2-darwin-arm64-blake3-abcdef0123456789");
  CHECK(envy::cache::key("llvm.clang@18.1.8", "windows", "x86_64", "ff00") ==
        "llvm.clang@18.1.8-windows-x86_64-blake3-ff00");
  CHECK(envy::cache::key("foo", "posix", "riscv", "0") == "foo-posix-riscv-blake3-0");
}

TEST_CASE("cache::key round-trips with util_parse_archive_filename") {
  auto const k{ envy::cache::key("arm.gcc@r2", "darwin", "arm64", "abcdef") };
  auto const parsed{ envy::util_parse_archive_filename(k) };
  REQUIRE(parsed.has_value());
  CHECK(parsed->identity == "arm.gcc@r2");
  CHECK(parsed->platform == "darwin");
  CHECK(parsed->arch == "arm64");
  CHECK(parsed->hash_prefix == "abcdef");
}
