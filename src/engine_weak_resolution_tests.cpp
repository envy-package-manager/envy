#include "engine.h"

#include "cache.h"
#include "doctest.h"
#include "manifest.h"
#include "pkg_key.h"

#include <atomic>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace envy {
namespace {

// Run the spec to completion and report which of `lookups` the engine interned. The
// lookup happens here because the engine, not a returned map, is the record.
static std::set<std::string> run_pkg_from_file(std::string const &identity,
                                               fs::path const &spec_path,
                                               std::vector<std::string> const &lookups) {
  static std::atomic<int> counter{ 0 };
  fs::path const cache_root{ fs::temp_directory_path() /
                             ("envy-weak-unit-" + std::to_string(++counter)) };

  cache c{ cache_root };
  auto manifest_ptr{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                                    spec_path) };
  engine eng{ c, manifest_ptr.get() };

  pkg_cfg *cfg_ptr{ pkg_cfg::pool()->emplace(
      identity,
      pkg_cfg::local_source{ .file_path = spec_path },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{}) };

  eng.run_full({ cfg_ptr });

  std::set<std::string> found;
  for (auto const &id : lookups) {
    if (eng.find_exact(pkg_key{ id })) { found.insert(id); }
  }
  fs::remove_all(cache_root);
  return found;
}

}  // namespace

TEST_CASE("weak reference resolves to an existing provider") {
  fs::path const spec_path{ "test_data/specs/weak_consumer_ref_only.lua" };
  auto const found{ run_pkg_from_file(
      "local.weak_consumer_ref_only@v1",
      spec_path,
      { "local.weak_consumer_ref_only@v1", "local.weak_provider@v1" }) };

  CHECK(found.contains("local.weak_consumer_ref_only@v1"));
  CHECK(found.contains("local.weak_provider@v1"));
}

TEST_CASE("weak dependency uses fallback when no match exists") {
  fs::path const spec_path{ "test_data/specs/weak_consumer_fallback.lua" };
  auto const found{ run_pkg_from_file(
      "local.weak_consumer_fallback@v1",
      spec_path,
      { "local.weak_consumer_fallback@v1", "local.weak_fallback@v1" }) };

  CHECK(found.contains("local.weak_consumer_fallback@v1"));
  CHECK(found.contains("local.weak_fallback@v1"));
}

TEST_CASE("weak dependency prefers existing match over fallback") {
  fs::path const spec_path{ "test_data/specs/weak_consumer_existing.lua" };
  auto const found{ run_pkg_from_file("local.weak_consumer_existing@v1",
                                      spec_path,
                                      { "local.weak_consumer_existing@v1",
                                        "local.existing_dep@v1",
                                        "local.unused_fallback@v1" }) };

  CHECK(found.contains("local.weak_consumer_existing@v1"));
  CHECK(found.contains("local.existing_dep@v1"));
  CHECK(!found.contains("local.unused_fallback@v1"));
}

TEST_CASE("ambiguity surfaces an error with both candidates listed") {
  fs::path const spec_path{ "test_data/specs/weak_consumer_ambiguous.lua" };
  try {
    run_pkg_from_file("local.weak_consumer_ambiguous@v1", spec_path, {});
    CHECK(false);  // Should not reach
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("ambiguous") != std::string::npos);
    CHECK(msg.find("local.dupe@v1") != std::string::npos);
    CHECK(msg.find("local.dupe@v2") != std::string::npos);
  }
}

TEST_CASE("reference-only dependency reports error when graph makes no progress") {
  fs::path const spec_path{ "test_data/specs/weak_missing_ref.lua" };
  try {
    run_pkg_from_file("local.weak_missing_ref@v1", spec_path, {});
    CHECK(false);
  } catch (std::runtime_error const &e) {
    std::string const msg{ e.what() };
    CHECK(msg.find("local.never_provided") != std::string::npos);
    CHECK(msg.find("no progress") != std::string::npos);
  }
}

TEST_CASE("weak fallbacks resolve across multiple iterations") {
  fs::path const spec_path{ "test_data/specs/weak_chain_root.lua" };
  auto const found{ run_pkg_from_file(
      "local.weak_chain_root@v1",
      spec_path,
      { "local.weak_chain_root@v1", "local.chain_b@v1", "local.chain_c@v1" }) };

  CHECK(found.contains("local.weak_chain_root@v1"));
  CHECK(found.contains("local.chain_b@v1"));
  CHECK(found.contains("local.chain_c@v1"));
}

TEST_CASE("reference-only resolution succeeds after fallbacks grow the graph") {
  fs::path const spec_path{ "test_data/specs/weak_progress_flat_root.lua" };
  auto const found{ run_pkg_from_file("local.weak_progress_flat_root@v1",
                                      spec_path,
                                      { "local.weak_progress_flat_root@v1",
                                        "local.branch_one@v1",
                                        "local.branch_two@v1",
                                        "local.shared@v1" }) };

  CHECK(found.contains("local.weak_progress_flat_root@v1"));
  CHECK(found.contains("local.branch_one@v1"));
  CHECK(found.contains("local.branch_two@v1"));
  CHECK(found.contains("local.shared@v1"));
}

}  // namespace envy
