#include "engine.h"

#include "cache.h"
#include "doctest.h"
#include "manifest.h"
#include "package_depot.h"
#include "pkg.h"
#include "platform.h"

#include <filesystem>
#include <vector>

namespace envy {

TEST_CASE("engine_validate_dependency_cycle: no cycle") {
  std::vector<std::string> const ancestors{ "A", "B", "C" };
  CHECK_NOTHROW(engine_validate_dependency_cycle("D", ancestors, "C", "Dependency"));
}

TEST_CASE("engine_validate_dependency_cycle: direct self-loop") {
  std::vector<std::string> const ancestors{ "A", "B" };
  CHECK_THROWS_WITH(engine_validate_dependency_cycle("C", ancestors, "C", "Dependency"),
                    "Dependency cycle detected: C -> C");
}

TEST_CASE("engine_validate_dependency_cycle: cycle in ancestor chain") {
  std::vector<std::string> const ancestors{ "A", "B", "C" };
  CHECK_THROWS_WITH(engine_validate_dependency_cycle("B", ancestors, "D", "Dependency"),
                    "Dependency cycle detected: B -> C -> D -> B");
}

TEST_CASE("engine_validate_dependency_cycle: cycle at chain start") {
  std::vector<std::string> const ancestors{ "A", "B", "C" };
  CHECK_THROWS_WITH(engine_validate_dependency_cycle("A", ancestors, "D", "Dependency"),
                    "Dependency cycle detected: A -> B -> C -> D -> A");
}

TEST_CASE("engine_validate_dependency_cycle: fetch dependency error message") {
  std::vector<std::string> const ancestors{ "A", "B" };
  CHECK_THROWS_WITH(
      engine_validate_dependency_cycle("A", ancestors, "C", "Fetch dependency"),
      "Fetch dependency cycle detected: A -> B -> C -> A");
}

TEST_CASE("engine_validate_dependency_cycle: empty ancestor chain with self-loop") {
  std::vector<std::string> const ancestors{};
  CHECK_THROWS_WITH(engine_validate_dependency_cycle("A", ancestors, "A", "Dependency"),
                    "Dependency cycle detected: A -> A");
}

TEST_CASE("engine_validate_dependency_cycle: empty ancestor chain without cycle") {
  std::vector<std::string> const ancestors{};
  CHECK_NOTHROW(engine_validate_dependency_cycle("B", ancestors, "A", "Dependency"));
}

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
  eng.mark_depot_bootstrap(p);
  CHECK(p->depot_bootstrap.load());
  CHECK(eng.depot_index_for(p) == nullptr);  // Returns before starting depot task

  fs::remove_all(cache_root);
}

TEST_CASE("mark_depot_bootstrap: propagates through dependency closure") {
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

  CHECK_FALSE(gn->depot_bootstrap.load());
  CHECK_FALSE(ninja_matches[0]->depot_bootstrap.load());

  eng.mark_depot_bootstrap(gn);

  CHECK(gn->depot_bootstrap.load());
  CHECK(ninja_matches[0]->depot_bootstrap.load());

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
  CHECK(gn_matches[0]->depot_bootstrap.load());
  auto const ninja_matches{ eng.find_matches("local.ninja@r0") };
  REQUIRE(ninja_matches.size() == 1);
  CHECK(ninja_matches[0]->depot_bootstrap.load());

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

// -- mark_fetch_closure -----------------------------------------------------
//
// A source.dependencies closure runs its whole phase ladder during graph
// resolution, so nothing in it may hold a weak reference. Two checks enforce that,
// one per order in which a package can enter a closure relative to its own
// spec_fetch: wire_dependency_graph catches marked-then-declares, and this catches
// declares-then-marked (a root that finished spec_fetch before something named it
// in source.dependencies). The second order is a thread race, so it is covered
// here rather than by a functional test that would have to win that race --
// measured, the declaration-time check wins it 20 times out of 20.

namespace {

std::unique_ptr<pkg> make_bare_pkg(std::string identity) {
  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(std::move(identity),
                                         pkg_cfg::weak_ref{},
                                         "{}",
                                         std::nullopt,
                                         nullptr,
                                         nullptr,
                                         std::vector<pkg_cfg *>{},
                                         std::nullopt,
                                         std::filesystem::path{}) };
  return std::unique_ptr<pkg>(new pkg{ .key = pkg_key(*cfg),
                                       .cfg = cfg,
                                       .cache_ptr = nullptr,
                                       .default_shell_ptr = nullptr,
                                       .tui_section = {},
                                       .lua = nullptr,
                                       .lock = nullptr,
                                       .canonical_identity_hash = {},
                                       .pkg_path = {},
                                       .spec_file_path = std::nullopt,
                                       .result_hash = {},
                                       .type = pkg_type::CACHE_MANAGED });
}

struct mark_closure_fixture {
  std::filesystem::path cache_root;
  cache c;
  engine eng;

  explicit mark_closure_fixture(char const *name)
      : cache_root{ std::filesystem::temp_directory_path() / name },
        c{ cache_root },
        eng{ c } {}
  ~mark_closure_fixture() { std::filesystem::remove_all(cache_root); }
};

}  // namespace

TEST_CASE("mark_fetch_closure: unresolved weak reference is rejected") {
  mark_closure_fixture fx{ "envy-mark-closure-unresolved" };
  auto p{ make_bare_pkg("local.p@v1") };
  p->weak_references.push_back(pkg::weak_reference{ .query = "wk", .is_product = true });

  CHECK_THROWS_WITH(fx.eng.mark_fetch_closure(p.get()),
                    doctest::Contains("must use strong dependencies"));
  CHECK(p->fetch_closure);  // flag is set before the throw; marking is not retried
}

TEST_CASE("mark_fetch_closure: already-resolved weak reference is accepted") {
  // Resolved at an earlier barrier iteration means the provider is wired and
  // ordered, so running early violates nothing. Throwing here would be a false
  // positive on a legal graph.
  mark_closure_fixture fx{ "envy-mark-closure-resolved" };
  auto provider{ make_bare_pkg("local.prov@v1") };
  auto p{ make_bare_pkg("local.p@v1") };
  p->weak_references.push_back(pkg::weak_reference{ .query = "wk",
                                                    .resolved = provider.get(),
                                                    .is_product = true });

  CHECK_NOTHROW(fx.eng.mark_fetch_closure(p.get()));
  CHECK(p->fetch_closure);
}

TEST_CASE("mark_fetch_closure: propagates to the transitive closure") {
  mark_closure_fixture fx{ "envy-mark-closure-transitive" };
  auto p{ make_bare_pkg("local.p@v1") };
  auto mid{ make_bare_pkg("local.mid@v1") };
  auto leaf{ make_bare_pkg("local.leaf@v1") };
  p->dependencies["local.mid@v1"] = { mid.get(), pkg_phase::pkg_build };
  mid->dependencies["local.leaf@v1"] = { leaf.get(), pkg_phase::pkg_build };

  CHECK_NOTHROW(fx.eng.mark_fetch_closure(p.get()));
  CHECK(p->fetch_closure);
  CHECK(mid->fetch_closure);
  CHECK(leaf->fetch_closure);
}

TEST_CASE("mark_fetch_closure: rejects a weak reference held deeper in the closure") {
  mark_closure_fixture fx{ "envy-mark-closure-deep" };
  auto p{ make_bare_pkg("local.p@v1") };
  auto leaf{ make_bare_pkg("local.leaf@v1") };
  p->dependencies["local.leaf@v1"] = { leaf.get(), pkg_phase::pkg_build };
  leaf->weak_references.push_back(pkg::weak_reference{ .query = "deep" });

  CHECK_THROWS_WITH(fx.eng.mark_fetch_closure(p.get()),
                    doctest::Contains("local.leaf@v1"));
}

TEST_CASE("mark_fetch_closure: is idempotent and terminates on a dependency cycle") {
  mark_closure_fixture fx{ "envy-mark-closure-cycle" };
  auto a{ make_bare_pkg("local.a@v1") };
  auto b{ make_bare_pkg("local.b@v1") };
  a->dependencies["local.b@v1"] = { b.get(), pkg_phase::pkg_build };
  b->dependencies["local.a@v1"] = { a.get(), pkg_phase::pkg_build };

  CHECK_NOTHROW(fx.eng.mark_fetch_closure(a.get()));
  CHECK(a->fetch_closure);
  CHECK(b->fetch_closure);
  CHECK_NOTHROW(fx.eng.mark_fetch_closure(a.get()));  // second call is a no-op
}

}  // namespace envy
