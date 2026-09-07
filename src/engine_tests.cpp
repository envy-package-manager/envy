#include "engine.h"

#include "cache.h"
#include "doctest.h"
#include "manifest.h"
#include "package_depot.h"
#include "pkg.h"
#include "platform.h"
#include "shell.h"

#include <filesystem>
#include <memory>
#include <variant>
#include <vector>

namespace envy {

TEST_CASE("engine_extend_dependencies: extends full closure") {
  // Setup: gn -> ninja -> python (chain), plus unrelated uv
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-extend-deps-test-1" };

  cache c{ cache_root };
  auto m{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                         fs::path("test_data/specs/dependency_chain_gn.lua")) };
  engine eng{ c, m.get() };

  // Create cfgs for gn, ninja, python, uv
  std::vector<pkg_cfg const *> roots;
  pkg_cfg *gn_cfg = pkg_cfg::pool()->emplace(
      "local.gn@r0",
      pkg_cfg::local_source{ .file_path =
                                 fs::path("test_data/specs/dependency_chain_gn.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      fs::path{});
  pkg_cfg *uv_cfg = pkg_cfg::pool()->emplace(
      "local.uv@r0",
      pkg_cfg::local_source{ .file_path = fs::path("test_data/specs/simple_uv.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      fs::path{});
  roots.push_back(gn_cfg);
  roots.push_back(uv_cfg);

  // resolve_graph starts all at spec_fetch
  eng.resolve_graph(roots);

  // All should be at spec_fetch after resolve
  CHECK(eng.get_pkg_target_phase(pkg_key(*gn_cfg)) == pkg_phase::spec_fetch);
  CHECK(eng.get_pkg_target_phase(pkg_key(*uv_cfg)) == pkg_phase::spec_fetch);

  // Find gn package
  pkg *gn_pkg = eng.find_exact(pkg_key(*gn_cfg));
  REQUIRE(gn_pkg != nullptr);

  // Extend gn's closure
  eng.extend_dependencies_to_completion(gn_pkg);

  // gn and its dependencies should be at completion
  CHECK(eng.get_pkg_target_phase(pkg_key(*gn_cfg)) == pkg_phase::completion);
  // ninja and python should also be extended (they're gn's dependencies)

  // uv should still be at spec_fetch (not in gn's closure)
  CHECK(eng.get_pkg_target_phase(pkg_key(*uv_cfg)) == pkg_phase::spec_fetch);

  fs::remove_all(cache_root);
}

TEST_CASE("engine_extend_dependencies: leaf package only extends itself") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-extend-deps-test-2" };

  cache c{ cache_root };
  auto m{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                         fs::path("test_data/specs/dependency_chain_gn.lua")) };
  engine eng{ c, m.get() };

  std::vector<pkg_cfg const *> roots;
  pkg_cfg *gn_cfg = pkg_cfg::pool()->emplace(
      "local.gn@r0",
      pkg_cfg::local_source{ .file_path =
                                 fs::path("test_data/specs/dependency_chain_gn.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      fs::path{});
  pkg_cfg *python_cfg = pkg_cfg::pool()->emplace(
      "local.python@r0",
      pkg_cfg::local_source{ .file_path = fs::path("test_data/specs/simple_python.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      fs::path{});
  roots.push_back(gn_cfg);
  roots.push_back(python_cfg);

  eng.resolve_graph(roots);

  // Find python (leaf with no dependencies)
  pkg *python_pkg = eng.find_exact(pkg_key(*python_cfg));
  REQUIRE(python_pkg != nullptr);

  // Extend python
  eng.extend_dependencies_to_completion(python_pkg);

  // Only python should be at completion
  CHECK(eng.get_pkg_target_phase(pkg_key(*python_cfg)) == pkg_phase::completion);

  // gn should still be at spec_fetch (not python's dependency)
  CHECK(eng.get_pkg_target_phase(pkg_key(*gn_cfg)) == pkg_phase::spec_fetch);

  fs::remove_all(cache_root);
}

TEST_CASE("resolve_graph: spec_fetch failures are propagated") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-resolve-fail-test" };

  cache c{ cache_root };
  auto m{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                         fs::path("test_data/specs/simple_python.lua")) };
  engine eng{ c, m.get() };

  pkg_cfg *bad_cfg = pkg_cfg::pool()->emplace(
      "local.nonexistent@v1",
      pkg_cfg::local_source{ .file_path = fs::path("test_data/specs/DOES_NOT_EXIST.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      fs::path{});

  std::vector<pkg_cfg const *> roots{ bad_cfg };
  CHECK_THROWS_WITH(eng.resolve_graph(roots), doctest::Contains("Spec source not found"));

  fs::remove_all(cache_root);
}

TEST_CASE("process_fetch_dependencies: manifest bundle parent stays null") {
  // When a manifest-declared bundle (with custom fetch) is used as a source_dependency,
  // its parent should remain nullptr because the fetch function is in the manifest,
  // not in the parent spec's Lua state.
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-bundle-parent-test" };

  cache c{ cache_root };
  auto m{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                         fs::path("test_data/specs/simple_python.lua")) };
  engine eng{ c, m.get() };

  // Create a bundle pkg_cfg like manifest would create it (with parent = nullptr)
  pkg_cfg *bundle_cfg = pkg_cfg::pool()->emplace(
      "test.custom-bundle@v1",  // identity = bundle identity
      pkg_cfg::bundle_source{ .bundle_identity = "test.custom-bundle@v1",
                              .fetch_source = pkg_cfg::custom_fetch_source{} },
      "{}",
      std::nullopt,
      nullptr,  // parent = nullptr (manifest-declared)
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      fs::path{});
  bundle_cfg->bundle_identity = "test.custom-bundle@v1";  // Mark as bundle

  // Create a spec that has the bundle as a source_dependency
  pkg_cfg *spec_cfg = pkg_cfg::pool()->emplace(
      "test.spec@v1",
      pkg_cfg::local_source{ .file_path = fs::path("test_data/specs/simple_python.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{ bundle_cfg },  // bundle as source_dependency
      std::nullopt,
      fs::path{});

  // Verify bundle parent is null before
  CHECK(bundle_cfg->parent == nullptr);

  // Run resolve_graph which will call process_fetch_dependencies
  std::vector<pkg_cfg const *> roots{ spec_cfg };

  // The resolve_graph will fail because the bundle can't actually be fetched,
  // but we can verify the parent wasn't modified before the failure
  try {
    eng.resolve_graph(roots);
  } catch (...) {
    // Expected to fail
  }

  // Verify bundle parent is STILL null (not set to spec_cfg)
  // This is the key assertion - manifest bundles keep nullptr parent
  CHECK(bundle_cfg->parent == nullptr);

  fs::remove_all(cache_root);
}

// --- engine_filter_host_platform ---

TEST_CASE("engine_filter_host_platform: empty platforms passes through") {
  pkg_cfg *cfg = pkg_cfg::pool()->emplace(
      "local.tool@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});

  auto result{ engine_filter_host_platform({ cfg }) };
  CHECK(result.size() == 1);
  CHECK(result[0] == cfg);
}

TEST_CASE("engine_filter_host_platform: current OS passes through") {
  pkg_cfg *cfg = pkg_cfg::pool()->emplace(
      "local.tool@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  cfg->platforms.push_back(std::string(platform::os_name()));

  auto result{ engine_filter_host_platform({ cfg }) };
  CHECK(result.size() == 1);
}

TEST_CASE("engine_filter_host_platform: other OS is filtered out") {
  std::string const other{ std::string(platform::os_name()) == "darwin" ? "linux"
                                                                        : "darwin" };

  pkg_cfg *cfg = pkg_cfg::pool()->emplace(
      "local.tool@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  cfg->platforms.push_back(other);

  auto result{ engine_filter_host_platform({ cfg }) };
  CHECK(result.empty());
}

TEST_CASE("engine_filter_host_platform: current os-arch passes through") {
  std::string const exact{ std::string(platform::os_name()) + "-" +
                           std::string(platform::arch_name()) };

  pkg_cfg *cfg = pkg_cfg::pool()->emplace(
      "local.tool@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  cfg->platforms.push_back(exact);

  auto result{ engine_filter_host_platform({ cfg }) };
  CHECK(result.size() == 1);
}

TEST_CASE("engine_filter_host_platform: wrong arch is filtered out") {
  std::string const wrong{ std::string(platform::os_name()) + "-" +
                           (std::string(platform::arch_name()) == "arm64" ? "x86_64"
                                                                          : "arm64") };

  pkg_cfg *cfg = pkg_cfg::pool()->emplace(
      "local.tool@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  cfg->platforms.push_back(wrong);

  auto result{ engine_filter_host_platform({ cfg }) };
  CHECK(result.empty());
}

TEST_CASE("engine_filter_host_platform: mixed roots keeps only matching") {
  std::string const other{ std::string(platform::os_name()) == "darwin" ? "linux"
                                                                        : "darwin" };

  pkg_cfg *good = pkg_cfg::pool()->emplace(
      "local.good@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  good->platforms.push_back(std::string(platform::os_name()));

  pkg_cfg *bad = pkg_cfg::pool()->emplace(
      "local.bad@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  bad->platforms.push_back(other);

  auto result{ engine_filter_host_platform({ good, bad }) };
  REQUIRE(result.size() == 1);
  CHECK(result[0] == good);
}

TEST_CASE("engine_filter_host_platform: multiple platforms one matching") {
  pkg_cfg *cfg = pkg_cfg::pool()->emplace(
      "local.tool@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  cfg->platforms.push_back("darwin");
  cfg->platforms.push_back("linux");
  cfg->platforms.push_back("windows");

  auto result{ engine_filter_host_platform({ cfg }) };
  CHECK(result.size() == 1);
}

TEST_CASE("engine_filter_host_platform: all filtered yields empty") {
  std::string const other{ std::string(platform::os_name()) == "darwin" ? "windows"
                                                                        : "darwin" };

  pkg_cfg *a = pkg_cfg::pool()->emplace(
      "local.a@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  a->platforms.push_back(other);

  pkg_cfg *b = pkg_cfg::pool()->emplace(
      "local.b@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
  b->platforms.push_back(other);

  auto result{ engine_filter_host_platform({ a, b }) };
  CHECK(result.empty());
}

// --- package depot (#depot task, depot_bootstrap exemption) ---

namespace {

pkg_cfg *make_local_cfg(std::string identity, std::string spec_path) {
  return pkg_cfg::pool()->emplace(
      std::move(identity),
      pkg_cfg::local_source{ .file_path = std::filesystem::path(std::move(spec_path)) },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});
}

}  // namespace

TEST_CASE("depot_index_for: no configured depots yields nullptr") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-none" };
  cache c{ cache_root };
  auto m{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  CHECK(eng.depot_index_for(p) == nullptr);

  fs::remove_all(cache_root);
}

TEST_CASE("depot_index_for: ignore-depot yields nullptr and spawns no deps") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-ignore" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = { { spec = "local.tool@r0", source = "test_data/specs/simple_python.lua" } }
PACKAGE_DEPOTS = {
  { DEPENDS = { "local.tool@r0" }, FETCH = function(ctx) return {} end },
}
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };
  eng.set_ignore_depot(true);

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  CHECK(eng.depot_index_for(p) == nullptr);
  CHECK(eng.find_matches("local.tool").empty());  // Depot dep never spawned

  fs::remove_all(cache_root);
}

TEST_CASE("depot_index_for: pre-set index bypasses depot task") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-preset" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = { { spec = "local.tool@r0", source = "test_data/specs/simple_python.lua" } }
PACKAGE_DEPOTS = {
  { DEPENDS = { "local.tool@r0" }, FETCH = function(ctx) error("must not run") end },
}
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };
  eng.set_depot_index(package_depot_index::build_from_contents(
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n" }));

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  auto const *depot{ eng.depot_index_for(p) };
  REQUIRE(depot != nullptr);
  CHECK(depot->find("pkg@v1", "darwin", "arm64", "aaaa").has_value());
  CHECK(eng.find_matches("local.tool").empty());  // Depot dep never spawned

  fs::remove_all(cache_root);
}

TEST_CASE("depot_index_for: depot-bootstrap package is exempt") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-exempt" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { FETCH = function(ctx) error("must not run") end } }
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  eng.mark_closure(p, pkg_closure::depot_bootstrap);
  CHECK(p->in_closure(pkg_closure::depot_bootstrap));
  CHECK(eng.depot_index_for(p) == nullptr);  // Returns before starting depot task

  fs::remove_all(cache_root);
}

TEST_CASE("mark_closure: depot_bootstrap propagates through dependency closure") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-mark" };
  cache c{ cache_root };
  auto m{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  pkg_cfg *gn_cfg{ make_local_cfg("local.gn@r0",
                                  "test_data/specs/dependency_chain_gn.lua") };
  eng.resolve_graph({ gn_cfg });  // gn depends on local.ninja@r0

  pkg *gn{ eng.find_exact(pkg_key(*gn_cfg)) };
  REQUIRE(gn != nullptr);
  auto const ninja_matches{ eng.find_matches("local.ninja@r0") };
  REQUIRE(ninja_matches.size() == 1);

  CHECK_FALSE(gn->in_closure(pkg_closure::depot_bootstrap));
  CHECK_FALSE(ninja_matches[0]->in_closure(pkg_closure::depot_bootstrap));

  eng.mark_closure(gn, pkg_closure::depot_bootstrap);

  CHECK(gn->in_closure(pkg_closure::depot_bootstrap));
  CHECK(ninja_matches[0]->in_closure(pkg_closure::depot_bootstrap));

  fs::remove_all(cache_root);
}

TEST_CASE("depot task: FETCH entries publish a merged index") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-entries" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  {
    FETCH = function(ctx)
      return {
        { url = "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst",
          sha256 = string.rep("a", 64) },
      }
    end,
  },
}
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  auto const *depot{ eng.depot_index_for(p) };  // Blocks until #depot publishes
  REQUIRE(depot != nullptr);
  auto const entry{ depot->find("pkg@v1", "darwin", "arm64", "aaaa") };
  REQUIRE(entry.has_value());
  CHECK(entry->url == "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst");

  fs::remove_all(cache_root);
}

TEST_CASE("depot task: FETCH raw text parses without SHA256 requirement") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-text" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  { FETCH = function(ctx) return "/local/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n" end },
}
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  auto const *depot{ eng.depot_index_for(p) };
  REQUIRE(depot != nullptr);
  auto const entry{ depot->find("pkg@v1", "darwin", "arm64", "aaaa") };
  REQUIRE(entry.has_value());
  CHECK_FALSE(entry->sha256.has_value());

  fs::remove_all(cache_root);
}

TEST_CASE("depot task: FETCH failure is fatal for waiting importers") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-fail" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { FETCH = function(ctx) error("boom") end } }
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  CHECK_THROWS_WITH_AS(eng.depot_index_for(p),
                       doctest::Contains("FETCH failed"),
                       std::runtime_error);

  fs::remove_all(cache_root);
}

TEST_CASE("depot task: unknown DEPENDS identity is fatal") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-missing" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  { DEPENDS = { "local.missing@v1" }, FETCH = function(ctx) return {} end },
}
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  CHECK_THROWS_WITH_AS(eng.depot_index_for(p),
                       doctest::Contains("not found in manifest"),
                       std::runtime_error);

  fs::remove_all(cache_root);
}

TEST_CASE("depot task: DEPENDS spawn as depot-bootstrap and feed ctx.deps") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-depot-eng-deps" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {
  { spec = "local.gn@r0", source = "test_data/specs/dependency_chain_gn.lua" },
}
PACKAGE_DEPOTS = {
  {
    DEPENDS = { "local.gn@r0" },
    FETCH = function(ctx)
      assert(ctx.deps["local.gn@r0"] ~= nil)
      return ""
    end,
  },
}
)",
                         fs::current_path() / "envy.lua") };
  engine eng{ c, m.get() };

  pkg *p{ eng.ensure_pkg(make_local_cfg("local.a@r0", "test_data/specs/simple_uv.lua")) };
  auto const *depot{ eng.depot_index_for(p) };  // Waits on gn through setup
  REQUIRE(depot != nullptr);
  CHECK(depot->empty());

  // The tool and its transitive dependency are flagged depot-bootstrap.
  auto const gn_matches{ eng.find_matches("local.gn@r0") };
  REQUIRE(gn_matches.size() == 1);
  CHECK(gn_matches[0]->in_closure(pkg_closure::depot_bootstrap));
  auto const ninja_matches{ eng.find_matches("local.ninja@r0") };
  REQUIRE(ninja_matches.size() == 1);
  CHECK(ninja_matches[0]->in_closure(pkg_closure::depot_bootstrap));

  fs::remove_all(cache_root);
}

TEST_CASE("engine_filter_host_platform: empty input yields empty output") {
  auto result{ engine_filter_host_platform({}) };
  CHECK(result.empty());
}

TEST_CASE("engine_filter_host_platform: preserves order of matching cfgs") {
  pkg_cfg *a = pkg_cfg::pool()->emplace(
      "local.a@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});

  pkg_cfg *b = pkg_cfg::pool()->emplace(
      "local.b@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});

  pkg_cfg *c = pkg_cfg::pool()->emplace(
      "local.c@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path("dummy.lua") },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      std::filesystem::path{});

  auto result{ engine_filter_host_platform({ a, b, c }) };
  REQUIRE(result.size() == 3);
  CHECK(result[0] == a);
  CHECK(result[1] == b);
  CHECK(result[2] == c);
}

// -- mark_closure -----------------------------------------------------
//
// A source.dependencies closure runs its whole phase ladder during graph
// resolution, so nothing in it may hold a weak reference. Two checks enforce that,
// one per order in which a package can enter a closure relative to its own
// spec_fetch: wire_dependency_graph catches marked-then-declares, and mark_closure catches
// declares-then-marked (a root that finished spec_fetch before something named it
// in source.dependencies). The second order is a thread race, so it is covered
// here rather than by a functional test that would have to win that race --
// measured, the declaration-time check wins it 20 times out of 20.

namespace {

std::unique_ptr<pkg> make_bare_pkg(std::string identity, std::string options = "{}") {
  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(std::move(identity),
                                         pkg_cfg::weak_ref{},
                                         std::move(options),
                                         std::nullopt,
                                         nullptr,
                                         nullptr,
                                         std::vector<pkg_cfg *>{},
                                         std::nullopt,
                                         std::filesystem::path{}) };
  return std::unique_ptr<pkg>(new pkg{ .key = pkg_key(*cfg),
                                       .cfg = cfg,
                                       .cache_ptr = nullptr,
                                       .eng = nullptr,
                                       .tui_section = {},
                                       .lua = nullptr,
                                       .lock = nullptr,
                                       .canonical_identity_hash = {},
                                       .pkg_path = {},
                                       .spec_file_path = std::nullopt,
                                       .result_hash = {},
                                       .type = pkg_type::CACHE_MANAGED });
}

// mark_closure touches no engine or cache state, so the cache root is inert:
// the constructor only stores the path, and nothing here creates or removes it.
struct mark_closure_fixture {
  cache c{ std::filesystem::path{ "envy-unit-test-inert-cache-root" } };
  engine eng{ c };
};

}  // namespace

TEST_CASE("mark_closure: unresolved weak reference is rejected") {
  mark_closure_fixture fx;
  auto p{ make_bare_pkg("local.p@v1") };
  p->weak_references.push_back(pkg::weak_reference{ .query = "wk", .is_product = true });

  CHECK_THROWS_WITH(fx.eng.mark_closure(p.get(), pkg_closure::fetch),
                    doctest::Contains("must use strong dependencies"));
  CHECK(p->in_closure(
      pkg_closure::fetch));  // flag is set before the throw; marking is not retried
}

TEST_CASE("mark_closure: already-resolved weak reference is accepted") {
  // Resolved at an earlier barrier iteration means the provider is wired and
  // ordered, so running early violates nothing. Throwing here would be a false
  // positive on a legal graph.
  mark_closure_fixture fx;
  auto provider{ make_bare_pkg("local.prov@v1") };
  auto p{ make_bare_pkg("local.p@v1") };
  p->weak_references.push_back(pkg::weak_reference{ .query = "wk",
                                                    .resolved = provider.get(),
                                                    .is_product = true });

  CHECK_NOTHROW(fx.eng.mark_closure(p.get(), pkg_closure::fetch));
  CHECK(p->in_closure(pkg_closure::fetch));
}

TEST_CASE("mark_closure: propagates to the transitive closure") {
  mark_closure_fixture fx;
  auto p{ make_bare_pkg("local.p@v1") };
  auto mid{ make_bare_pkg("local.mid@v1") };
  auto leaf{ make_bare_pkg("local.leaf@v1") };
  p->dependencies["local.mid@v1"] = { mid.get(), pkg_phase::pkg_build };
  mid->dependencies["local.leaf@v1"] = { leaf.get(), pkg_phase::pkg_build };

  CHECK_NOTHROW(fx.eng.mark_closure(p.get(), pkg_closure::fetch));
  CHECK(p->in_closure(pkg_closure::fetch));
  CHECK(mid->in_closure(pkg_closure::fetch));
  CHECK(leaf->in_closure(pkg_closure::fetch));
}

TEST_CASE("mark_closure: rejects a weak reference held deeper in the closure") {
  mark_closure_fixture fx;
  auto p{ make_bare_pkg("local.p@v1") };
  auto leaf{ make_bare_pkg("local.leaf@v1") };
  p->dependencies["local.leaf@v1"] = { leaf.get(), pkg_phase::pkg_build };
  leaf->weak_references.push_back(pkg::weak_reference{ .query = "deep" });

  CHECK_THROWS_WITH(fx.eng.mark_closure(p.get(), pkg_closure::fetch),
                    doctest::Contains("local.leaf@v1"));
}

TEST_CASE("mark_closure: is idempotent and terminates on a dependency cycle") {
  mark_closure_fixture fx;
  auto a{ make_bare_pkg("local.a@v1") };
  auto b{ make_bare_pkg("local.b@v1") };
  a->dependencies["local.b@v1"] = { b.get(), pkg_phase::pkg_build };
  b->dependencies["local.a@v1"] = { a.get(), pkg_phase::pkg_build };

  CHECK_NOTHROW(fx.eng.mark_closure(a.get(), pkg_closure::fetch));
  CHECK(a->in_closure(pkg_closure::fetch));
  CHECK(b->in_closure(pkg_closure::fetch));
  CHECK_NOTHROW(
      fx.eng.mark_closure(a.get(), pkg_closure::fetch));  // second call is a no-op
}

TEST_CASE("mark_closure: memberships are independent bits") {
  // One bitmask now carries both closures, so marking one must not imply or clear the
  // other, and a package legitimately in both must stay in both.
  mark_closure_fixture fx;
  auto p{ make_bare_pkg("local.p@v1") };

  fx.eng.mark_closure(p.get(), pkg_closure::fetch);
  CHECK(p->in_closure(pkg_closure::fetch));
  CHECK_FALSE(p->in_closure(pkg_closure::depot_bootstrap));

  fx.eng.mark_closure(p.get(), pkg_closure::depot_bootstrap);
  CHECK(p->in_closure(pkg_closure::fetch));
  CHECK(p->in_closure(pkg_closure::depot_bootstrap));
}

TEST_CASE("mark_closure: a bit already set does not stop the other propagating") {
  // The bit doubles as the visited set, so a closure that stops at an already-marked
  // node must still be able to walk past it for a *different* closure.
  mark_closure_fixture fx;
  auto root{ make_bare_pkg("local.root@v1") };
  auto leaf{ make_bare_pkg("local.leaf@v1") };
  root->dependencies["local.leaf@v1"] = { leaf.get(), pkg_phase::pkg_build };

  fx.eng.mark_closure(root.get(), pkg_closure::fetch);
  fx.eng.mark_closure(root.get(), pkg_closure::depot_bootstrap);

  CHECK(leaf->in_closure(pkg_closure::fetch));
  CHECK(leaf->in_closure(pkg_closure::depot_bootstrap));
}

// -- ensure_pkg / failure aggregation ---------------------------------

TEST_CASE("ensure_pkg: a repeat call reuses the package instead of building another") {
  // The duplicate construction was invisible except for the TUI row it burned:
  // handles only ever rise, so a second call must not consume one.
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-ensure-pkg-reuse" };
  cache c{ cache_root };
  engine eng{ c };

  pkg_cfg *cfg{ make_local_cfg("local.reuse@r0", "test_data/specs/simple_uv.lua") };

  tui::section_handle const before{ tui::section_create() };
  pkg *first{ eng.ensure_pkg(cfg) };
  pkg *second{ eng.ensure_pkg(cfg) };
  tui::section_handle const after{ tui::section_create() };
  tui::section_delete(before);
  tui::section_delete(after);

  CHECK(first == second);
  CHECK(first->tui_section == before + 1);
  CHECK(after == before + 2);
  CHECK(first->canonical_identity_hash == first->key.canonical());
  CHECK(first->eng == &eng);
  CHECK(first->cache_ptr == &c);
  CHECK(first->type == pkg_type::UNKNOWN);

  fs::remove_all(cache_root);
}

// -- ensure_pkg: two cfgs on one key must agree about what to fetch --------
//
// pkg_key holds identity and options, never the source, so the cfg that wins the
// insert would otherwise silently decide what gets downloaded.

namespace {

pkg_cfg *make_cfg_with_source(std::string identity,
                              pkg_cfg::source_t source,
                              std::filesystem::path declared_in) {
  return pkg_cfg::pool()->emplace(std::move(identity),
                                  std::move(source),
                                  "{}",
                                  std::nullopt,
                                  nullptr,
                                  nullptr,
                                  std::vector<pkg_cfg *>{},
                                  std::nullopt,
                                  std::move(declared_in));
}

struct ensure_pkg_fixture {
  std::filesystem::path root{ std::filesystem::temp_directory_path() /
                              "envy-ensure-pkg-sources" };
  cache c{ root };
  engine eng{ c };
  ~ensure_pkg_fixture() { std::filesystem::remove_all(root); }
};

}  // namespace

TEST_CASE("ensure_pkg: a remote and a local declaration of one key conflict") {
  ensure_pkg_fixture fx;
  auto *remote{ make_cfg_with_source("local.dup@r0",
                                     pkg_cfg::remote_source{ .url = "https://x/a.lua" },
                                     "/proj/one.lua") };
  auto *local{ make_cfg_with_source(
      "local.dup@r0",
      pkg_cfg::local_source{ .file_path = std::filesystem::path{ "/proj/a.lua" } },
      "/proj/two.lua") };

  REQUIRE(fx.eng.ensure_pkg(remote) != nullptr);
  CHECK_THROWS_WITH_AS(fx.eng.ensure_pkg(local),
                       doctest::Contains("conflicting sources in /proj/one.lua and "
                                         "/proj/two.lua"),
                       std::runtime_error);
}

TEST_CASE("ensure_pkg: two identical remote declarations agree") {
  ensure_pkg_fixture fx;
  auto *one{ make_cfg_with_source(
      "local.same@r0",
      pkg_cfg::remote_source{ .url = "https://x/a.lua", .sha256 = "ab" },
      "/proj/one.lua") };
  auto *two{ make_cfg_with_source(
      "local.same@r0",
      pkg_cfg::remote_source{ .url = "https://x/a.lua", .sha256 = "ab" },
      "/proj/two.lua") };

  pkg *first{ fx.eng.ensure_pkg(one) };
  CHECK(fx.eng.ensure_pkg(two) == first);
}

TEST_CASE("ensure_pkg: a reference-only declaration agrees with a concrete one") {
  ensure_pkg_fixture fx;
  auto *remote{ make_cfg_with_source("local.ref@r0",
                                     pkg_cfg::remote_source{ .url = "https://x/a.lua" },
                                     "/proj/one.lua") };
  auto *ref{ make_cfg_with_source("local.ref@r0", pkg_cfg::weak_ref{}, "/proj/two.lua") };

  pkg *first{ fx.eng.ensure_pkg(remote) };
  CHECK(fx.eng.ensure_pkg(ref) == first);
  CHECK(fx.eng.ensure_pkg(remote) == first);
}

namespace {

// How many times `needle` occurs in `haystack`. A message stored under two spellings
// (prefixed on one task, bare on another) survives the dedup and shows up twice.
size_t count_occurrences(std::string const &haystack, std::string_view needle) {
  size_t n{ 0 };
  for (size_t at{ haystack.find(needle) }; at != std::string::npos;
       at = haystack.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("run_full: reports every failure, each exactly once") {
  // Two unrelated failures must both surface, a dependent that re-stored its
  // dependency's message verbatim must not print it a second time, and neither must
  // a package whose two SETUP pairs both failed.
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-run-full-failures" };
  cache c{ cache_root };
  auto m{ manifest::load("-- @envy bin-dir \"tools\"\nPACKAGES = {}",
                         fs::path("test_data/specs/failing_setup_a.lua")) };
  engine eng{ c, m.get() };

  pkg_cfg *consumer{ make_local_cfg("local.failing_setup_consumer@r0",
                                    "test_data/specs/failing_setup_consumer.lua") };
  pkg_cfg *b{ make_local_cfg("local.failing_setup_b@r0",
                             "test_data/specs/failing_setup_b.lua") };
  b->setup = std::vector<std::string>{ "main" };
  pkg_cfg *two{ make_local_cfg("local.failing_setup_two@r0",
                               "test_data/specs/failing_setup_two_pairs.lua") };
  two->setup = std::vector<std::string>{ "one", "two" };

  std::string const msg{ [&]() -> std::string {
    try {
      eng.run_full({ consumer, b, two });
    } catch (std::exception const &e) { return e.what(); }
    return {};
  }() };

  auto const count{ [&](std::string_view needle) {
    return count_occurrences(msg, needle);
  } };

  CHECK(count("setup A refused") == 1);
  CHECK(count("setup B refused") == 1);
  CHECK(count("setup one refused") == 1);
  CHECK(count("setup two refused") == 1);

  fs::remove_all(cache_root);
}

TEST_CASE("run_full: a bootstrap task reports its failure exactly once") {
  // #default_shell stores the message its waiters get, not the bare one: otherwise the
  // task and every package that asked it for a shell hold two spellings of one failure.
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-run-full-shell-failure" };
  cache c{ cache_root };
  auto m{ manifest::load(
      "-- @envy bin-dir \"tools\"\nPACKAGES = {}\n"
      "DEFAULT_SHELL = function() error(\"shell refused\") end\n",
      fs::path("test_data/specs/string_verb_setup.lua")) };
  engine eng{ c, m.get() };

  pkg_cfg *p{ make_local_cfg("local.string_verb_setup@r0",
                             "test_data/specs/string_verb_setup.lua") };
  p->setup = std::vector<std::string>{ "main" };

  std::string const msg{ [&]() -> std::string {
    try {
      eng.run_full({ p });
    } catch (std::exception const &e) { return e.what(); }
    return {};
  }() };

  CHECK(count_occurrences(msg, "shell refused") == 1);

  fs::remove_all(cache_root);
}

TEST_CASE("mark_closure: depot_bootstrap rejects an unresolved weak reference too") {
  // Both closures run outside the weak-resolution window, so both refuse. Before the
  // two flags shared one mechanism, only the source.dependencies closure checked this
  // at mark time, and the message names which closure refused.
  mark_closure_fixture fx;
  auto p{ make_bare_pkg("local.p@v1") };
  p->weak_references.push_back(pkg::weak_reference{ .query = "wk" });

  CHECK_THROWS_WITH(fx.eng.mark_closure(p.get(), pkg_closure::depot_bootstrap),
                    doctest::Contains("package-depot dependency closure"));
}

// -- wire_dependency ---------------------------------------------------
//
// One entry per (parent, identity), at the earliest phase anyone asked for, and
// never an edge that closes a cycle. The fixture's packages are deliberately not
// interned: wire_dependency touches only the parent's edge map.

TEST_CASE("wire_dependency: a later, earlier needed_by tightens the edge") {
  mark_closure_fixture fx;
  auto parent{ make_bare_pkg("local.parent@v1") };
  auto dep{ make_bare_pkg("local.dep@v1") };

  fx.eng.wire_dependency(parent.get(), dep.get(), pkg_phase::pkg_install);
  fx.eng.wire_dependency(parent.get(), dep.get(), pkg_phase::pkg_stage);

  REQUIRE(parent->dependencies.size() == 1);
  CHECK(parent->dependencies.at("local.dep@v1").needed_by == pkg_phase::pkg_stage);
}

TEST_CASE("wire_dependency: a later, looser needed_by does not loosen the edge") {
  mark_closure_fixture fx;
  auto parent{ make_bare_pkg("local.parent@v1") };
  auto dep{ make_bare_pkg("local.dep@v1") };

  fx.eng.wire_dependency(parent.get(), dep.get(), pkg_phase::pkg_stage);
  fx.eng.wire_dependency(parent.get(), dep.get(), pkg_phase::pkg_install);

  CHECK(parent->dependencies.at("local.dep@v1").needed_by == pkg_phase::pkg_stage);
}

TEST_CASE("wire_dependency: one identity cannot map to two option variants") {
  mark_closure_fixture fx;
  auto parent{ make_bare_pkg("local.parent@v1") };
  auto one{ make_bare_pkg("local.dep@v1", "{v=1}") };
  auto two{ make_bare_pkg("local.dep@v1", "{v=2}") };

  fx.eng.wire_dependency(parent.get(), one.get(), pkg_phase::pkg_build);
  CHECK_THROWS_WITH(fx.eng.wire_dependency(parent.get(), two.get(), pkg_phase::pkg_build),
                    doctest::Contains("different options"));
  CHECK(parent->dependencies.at("local.dep@v1").p == one.get());
}

TEST_CASE("wire_dependency: a self edge is a cycle") {
  mark_closure_fixture fx;
  auto p{ make_bare_pkg("local.p@v1") };

  CHECK_THROWS_WITH(fx.eng.wire_dependency(p.get(), p.get(), pkg_phase::pkg_build),
                    "Dependency cycle detected: local.p@v1 -> local.p@v1");
}

TEST_CASE("wire_dependency: the error prints the whole cycle path") {
  // Reachability, not a spawn path: the back edge closes a cycle three hops long
  // that no single ancestor chain would have recorded.
  mark_closure_fixture fx;
  auto a{ make_bare_pkg("local.a@v1") };
  auto b{ make_bare_pkg("local.b@v1") };
  auto c{ make_bare_pkg("local.c@v1") };

  fx.eng.wire_dependency(a.get(), b.get(), pkg_phase::pkg_build);
  fx.eng.wire_dependency(b.get(), c.get(), pkg_phase::pkg_build);

  CHECK_THROWS_WITH(
      fx.eng.wire_dependency(c.get(), a.get(), pkg_phase::pkg_build, "Fetch dependency"),
      "Fetch dependency cycle detected: local.c@v1 -> local.a@v1 -> local.b@v1 -> "
      "local.c@v1");
  CHECK(c->dependencies.empty());
}

TEST_CASE("wire_dependency: a diamond is not a cycle") {
  mark_closure_fixture fx;
  auto a{ make_bare_pkg("local.a@v1") };
  auto b{ make_bare_pkg("local.b@v1") };
  auto c{ make_bare_pkg("local.c@v1") };
  auto d{ make_bare_pkg("local.d@v1") };

  fx.eng.wire_dependency(a.get(), b.get(), pkg_phase::pkg_build);
  fx.eng.wire_dependency(a.get(), c.get(), pkg_phase::pkg_build);
  fx.eng.wire_dependency(b.get(), d.get(), pkg_phase::pkg_build);
  CHECK_NOTHROW(fx.eng.wire_dependency(c.get(), d.get(), pkg_phase::pkg_build));
}

TEST_CASE("wire_dependency: closures reach the newly wired dependency") {
  mark_closure_fixture fx;
  auto parent{ make_bare_pkg("local.parent@v1") };
  auto dep{ make_bare_pkg("local.dep@v1") };
  fx.eng.mark_closure(parent.get(), pkg_closure::fetch);

  fx.eng.wire_dependency(parent.get(), dep.get(), pkg_phase::pkg_build);

  CHECK(dep->in_closure(pkg_closure::fetch));
}

// -- engine_resolve_targets --------------------------------------------

// --- DEFAULT_SHELL (bootstrap-shell rule) ---

namespace {

// The platform built-in, which is always a constant — a custom form would have to
// name an interpreter, and nothing has installed one at this point.
bool is_builtin_shell(resolved_shell const &s) {
  auto const *choice{ std::get_if<shell_choice>(&s) };
  return choice && *choice == std::get<shell_choice>(shell_resolve_default(nullptr));
}

// A function DEFAULT_SHELL that fails if it is ever evaluated: every case below must
// answer from the bootstrap rule alone, without running the manifest's Lua.
std::unique_ptr<manifest> exploding_shell_manifest() {
  return manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {}
DEFAULT_SHELL = { SHELL = function() error("shell must not be evaluated") end }
)",
                        std::filesystem::path("/fake/envy.lua"));
}

}  // namespace

TEST_CASE("default_shell: no manifest yields the platform built-in") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-ds-eng-none" };
  cache c{ cache_root };
  engine eng{ c };

  CHECK(is_builtin_shell(eng.default_shell(nullptr)));

  fs::remove_all(cache_root);
}

TEST_CASE("default_shell: a value form resolves without any task") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-ds-eng-value" };
  cache c{ cache_root };
  auto m{ manifest::load(R"(-- @envy bin-dir "tools"
PACKAGES = {}
DEFAULT_SHELL = ENVY_SHELL.SH
)",
                         fs::path("/fake/envy.lua")) };
  engine eng{ c, m.get() };

  auto const shell{ eng.default_shell(nullptr) };
  REQUIRE(std::holds_alternative<shell_choice>(shell));
  CHECK(std::get<shell_choice>(shell) == shell_choice::sh);

  fs::remove_all(cache_root);
}

TEST_CASE("default_shell: a null package gets the built-in, function form or not") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-ds-eng-null" };
  cache c{ cache_root };
  auto m{ exploding_shell_manifest() };
  engine eng{ c, m.get() };

  CHECK(is_builtin_shell(eng.default_shell(nullptr)));

  fs::remove_all(cache_root);
}

TEST_CASE("default_shell: a member of any bootstrap closure gets the built-in") {
  namespace fs = std::filesystem;
  fs::path const cache_root{ fs::temp_directory_path() / "envy-ds-eng-closure" };
  cache c{ cache_root };
  auto m{ exploding_shell_manifest() };
  engine eng{ c, m.get() };

  int i{ 0 };
  for (auto const kind :
       { pkg_closure::depot_bootstrap, pkg_closure::fetch, pkg_closure::default_shell }) {
    pkg *p{ eng.ensure_pkg(make_local_cfg("local.m" + std::to_string(i++) + "@r0",
                                          "test_data/specs/simple_uv.lua")) };
    eng.mark_closure(p, kind);
    CHECK(is_builtin_shell(eng.default_shell(p)));
  }

  fs::remove_all(cache_root);
}

TEST_CASE("engine_resolve_targets: an ambiguous query names its candidates") {
  // First-wins picked whichever entry the manifest happened to list first. Sorted,
  // so one manifest always produces one message.
  std::vector<pkg_cfg *> const packages{
    make_local_cfg("local.tool@v2", "dummy.lua"),
    make_local_cfg("local.tool@v1", "dummy.lua"),
  };

  CHECK_THROWS_WITH(engine_resolve_targets(packages, { "local.tool" }, "install"),
                    "install: query 'local.tool' is ambiguous: local.tool@v1, "
                    "local.tool@v2");

  auto const exact{ engine_resolve_targets(packages, { "local.tool@v1" }, "install") };
  REQUIRE(exact.size() == 1);
  CHECK(exact[0] == packages[1]);
}

TEST_CASE("engine_resolve_targets: duplicate declarations of one key are one target") {
  // envy.import splices subprojects' PACKAGES; two of them pinning the same shared
  // dependency collapse onto one pkg_key in ensure_pkg, so they are one package here too.
  std::vector<pkg_cfg *> const packages{
    make_local_cfg("local.tool@v1", "dummy.lua"),
    make_local_cfg("local.tool@v1", "dummy.lua"),
  };

  auto const targets{ engine_resolve_targets(packages, { "local.tool" }, "DEFAULT_SHELL") };
  REQUIRE(targets.size() == 1);
  CHECK(pkg_key{ *targets[0] }.canonical() == "local.tool@v1");
}

}  // namespace envy
