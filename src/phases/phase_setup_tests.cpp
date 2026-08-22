#include "phase_setup.h"

#include "cache.h"
#include "engine.h"
#include "lua_envy.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "sol_util.h"

#include "doctest.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace envy {

// Extern declarations for unit testing (not in public API)
extern bool run_pair_check(pkg *p, engine &eng, std::string const &name);
extern void run_pair_install(pkg *p,
                             engine &eng,
                             std::string const &name,
                             tui::section_handle section,
                             std::string const &log_identity);
extern std::vector<std::string> compute_selected_pairs(pkg *p);

namespace {

// Helper fixture for creating test packages with Lua states
struct setup_test_fixture {
  pkg_cfg *cfg;
  std::unique_ptr<pkg> p;

  setup_test_fixture() {
    cfg = pkg_cfg::pool()->emplace("test.package@v1",
                                   pkg_cfg::weak_ref{},
                                   "{}",
                                   std::nullopt,
                                   nullptr,
                                   nullptr,
                                   std::vector<pkg_cfg *>{},
                                   std::nullopt,
                                   std::filesystem::path{});

    // Create Lua state first
    auto lua_state = sol_util_make_lua_state();
    lua_envy_install(*lua_state);

    p = std::unique_ptr<pkg>(new pkg{ .key = pkg_key(*cfg),
                                      .cfg = cfg,
                                      .cache_ptr = nullptr,
                                      .eng = nullptr,
                                      .lua = std::move(lua_state),
                                      .lock = nullptr,
                                      .canonical_identity_hash = {},
                                      .pkg_path = std::filesystem::path{},
                                      .spec_file_path = std::nullopt,
                                      .result_hash = {},
                                      .type = pkg_type::USER_MANAGED,
                                      .declared_dependencies = {},
                                      .owned_dependency_cfgs = {},
                                      .dependencies = {},
                                      .product_dependencies = {},
                                      .weak_references = {},
                                      .products = {},
                                      .resolved_weak_dependency_keys = {} });
  }

  // Install a SETUP table via Lua source, e.g.
  // set_setup("{ main = { CHECK = 'exit 0', INSTALL = 'echo ok' } }")
  // Also mirrors pair names into p->setup_pairs as spec_fetch would.
  void set_setup(std::string_view setup_lua) {
    auto const acc{ p->lua.lock() };
    sol::state_view state{ *acc };
    std::string const code{ "SETUP = " + std::string(setup_lua) };
    sol::protected_function_result res{ state.safe_script(code,
                                                          sol::script_pass_on_error) };
    if (!res.valid()) {
      sol::error err = res;
      throw std::runtime_error(std::string("Failed to set SETUP: ") + err.what());
    }

    p->setup_pairs.clear();
    sol::table setup{ state["SETUP"].get<sol::table>() };
    for (auto const &[key, _] : setup) {
      p->setup_pairs.emplace(sol::object(key).as<std::string>(), pkg::setup_pair_decl{});
    }
  }

  void run_install(engine &eng, std::string const &name) {
    run_pair_install(p.get(), eng, name, p->tui_section, p->cfg->identity);
  }

  void set_options(std::string_view options_lua) {
    auto const acc{ p->lua.lock() };
    sol::state_view state{ *acc };
    sol::protected_function_result res{
      state.safe_script("return " + std::string(options_lua), sol::script_pass_on_error)
    };
    REQUIRE(res.valid());
    state.registry()[ENVY_OPTIONS_RIDX] = res.get<sol::object>();
  }
};

}  // namespace

// ============================================================================
// run_pair_check() - string form
// ============================================================================

TEST_CASE_FIXTURE(setup_test_fixture, "pair check string returns true on exit 0") {
  set_setup("{ main = { CHECK = 'exit 0', INSTALL = 'echo ok' } }");
  cache test_cache;
  engine eng{ test_cache };
  CHECK(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check string returns false on exit 1") {
  set_setup("{ main = { CHECK = 'exit 1', INSTALL = 'echo ok' } }");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_FALSE(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check string returns false on exit 42") {
  set_setup("{ main = { CHECK = 'exit 42', INSTALL = 'echo ok' } }");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_FALSE(run_pair_check(p.get(), eng, "main"));
}

// ============================================================================
// run_pair_check() - function form
// ============================================================================

TEST_CASE_FIXTURE(setup_test_fixture, "pair check function returns true") {
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) return true end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check function returns false") {
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) return false end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_FALSE(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture,
                  "pair check function returning string executes as shell") {
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) return 'exit 0' end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check function returning nil throws") {
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) return nil end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_THROWS_AS(run_pair_check(p.get(), eng, "main"), std::runtime_error);
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check function returning number throws") {
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) return 42 end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_THROWS_AS(run_pair_check(p.get(), eng, "main"), std::runtime_error);
}

TEST_CASE_FIXTURE(setup_test_fixture,
                  "pair check receives nil pkg_dir for user-managed packages") {
  p->type = pkg_type::USER_MANAGED;
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) return pkg_dir == nil end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture,
                  "pair check receives pkg_dir string for cache-managed packages") {
  p->type = pkg_type::CACHE_MANAGED;
  p->pkg_path = std::filesystem::path{ "/tmp/envy-test-pkg" };
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts)
      return type(pkg_dir) == 'string' and pkg_dir:find('envy%-test%-pkg') ~= nil
    end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check receives options table") {
  set_options("{ package = 'ghostty' }");
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts)
      return opts ~= nil and opts.package == 'ghostty'
    end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check returned string interpolates options") {
  set_options("{ exit_code = '0' }");
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) return 'exit ' .. opts.exit_code end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK(run_pair_check(p.get(), eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair check Lua error carries identity context") {
  cfg->identity = "my.package@v1";
  set_setup(R"({ main = {
    CHECK = function(pkg_dir, opts) error('something went wrong') end,
    INSTALL = 'echo ok',
  } })");
  cache test_cache;
  engine eng{ test_cache };

  try {
    run_pair_check(p.get(), eng, "main");
    FAIL("Expected exception");
  } catch (std::runtime_error const &e) {
    std::string msg{ e.what() };
    CHECK(msg.find("my.package@v1") != std::string::npos);
    CHECK(msg.find("something went wrong") != std::string::npos);
  }
}

// ============================================================================
// run_pair_install()
// ============================================================================

TEST_CASE_FIXTURE(setup_test_fixture, "pair install string executes") {
  set_setup("{ main = { CHECK = 'exit 1', INSTALL = 'echo installing' } }");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_NOTHROW(run_install(eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair install string non-zero exit throws") {
  set_setup("{ main = { CHECK = 'exit 1', INSTALL = 'exit 1' } }");
  cache test_cache;
  engine eng{ test_cache };

  try {
    run_install(eng, "main");
    FAIL("Expected exception");
  } catch (std::runtime_error const &e) {
    std::string msg{ e.what() };
    CHECK(msg.find("Setup shell script failed") != std::string::npos);
    CHECK(msg.find("exit code 1") != std::string::npos);
  }
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair install function returning nil succeeds") {
  set_setup(R"({ main = {
    CHECK = 'exit 1',
    INSTALL = function(pkg_dir, opts) end,
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_NOTHROW(run_install(eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture,
                  "pair install function returning string executes as shell") {
  set_setup(R"({ main = {
    CHECK = 'exit 1',
    INSTALL = function(pkg_dir, opts) return 'exit 0' end,
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_NOTHROW(run_install(eng, "main"));
}

TEST_CASE_FIXTURE(setup_test_fixture,
                  "pair install function returning failing string throws") {
  set_setup(R"({ main = {
    CHECK = 'exit 1',
    INSTALL = function(pkg_dir, opts) return 'exit 3' end,
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_THROWS_AS(run_install(eng, "main"), std::runtime_error);
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair install function returning number throws") {
  set_setup(R"({ main = {
    CHECK = 'exit 1',
    INSTALL = function(pkg_dir, opts) return 42 end,
  } })");
  cache test_cache;
  engine eng{ test_cache };

  try {
    run_install(eng, "main");
    FAIL("Expected exception");
  } catch (std::runtime_error const &e) {
    std::string msg{ e.what() };
    CHECK(msg.find("must return nil or string") != std::string::npos);
    CHECK(msg.find("number") != std::string::npos);
  }
}

TEST_CASE_FIXTURE(setup_test_fixture, "pair install receives options table") {
  set_options("{ flavor = 'mint' }");
  set_setup(R"({ main = {
    CHECK = 'exit 1',
    INSTALL = function(pkg_dir, opts)
      if opts.flavor ~= 'mint' then error('missing options') end
    end,
  } })");
  cache test_cache;
  engine eng{ test_cache };
  CHECK_NOTHROW(run_install(eng, "main"));
}

// ============================================================================
// compute_selected_pairs()
// ============================================================================

TEST_CASE_FIXTURE(setup_test_fixture,
                  "user-managed with no selection selects nothing (explicit-only)") {
  p->type = pkg_type::USER_MANAGED;
  p->setup_pairs.emplace("zeta", pkg::setup_pair_decl{});
  p->setup_pairs.emplace("alpha", pkg::setup_pair_decl{});

  CHECK(compute_selected_pairs(p.get()).empty());
}

TEST_CASE_FIXTURE(setup_test_fixture, "cache-managed with no selection selects nothing") {
  p->type = pkg_type::CACHE_MANAGED;
  p->setup_pairs.emplace("alpha", pkg::setup_pair_decl{});

  CHECK(compute_selected_pairs(p.get()).empty());
}

TEST_CASE_FIXTURE(setup_test_fixture, "explicit selection picks named pairs only") {
  p->type = pkg_type::CACHE_MANAGED;
  p->setup_pairs.emplace("alpha", pkg::setup_pair_decl{});
  p->setup_pairs.emplace("beta", pkg::setup_pair_decl{});
  p->setup_selected.insert("beta");

  auto const selected{ compute_selected_pairs(p.get()) };
  REQUIRE(selected.size() == 1);
  CHECK(selected[0] == "beta");
}

TEST_CASE_FIXTURE(setup_test_fixture, "selection closes transitively over DEPENDS") {
  p->type = pkg_type::USER_MANAGED;
  p->setup_pairs.emplace("a", pkg::setup_pair_decl{});
  p->setup_pairs.emplace("b", pkg::setup_pair_decl{ .platforms = {}, .depends = { "a" } });
  p->setup_pairs.emplace("c", pkg::setup_pair_decl{ .platforms = {}, .depends = { "b" } });
  p->setup_selected.insert("c");

  auto const selected{ compute_selected_pairs(p.get()) };
  REQUIRE(selected.size() == 3);
  CHECK(selected[0] == "a");
  CHECK(selected[1] == "b");
  CHECK(selected[2] == "c");
}

TEST_CASE_FIXTURE(setup_test_fixture, "diamond DEPENDS closure resolves each pair once") {
  p->type = pkg_type::CACHE_MANAGED;
  p->setup_pairs.emplace("a", pkg::setup_pair_decl{});
  p->setup_pairs.emplace("b", pkg::setup_pair_decl{ .platforms = {}, .depends = { "a" } });
  p->setup_pairs.emplace("c", pkg::setup_pair_decl{ .platforms = {}, .depends = { "a" } });
  p->setup_pairs.emplace("d",
                         pkg::setup_pair_decl{ .platforms = {}, .depends = { "b", "c" } });
  p->setup_selected.insert("d");

  auto const selected{ compute_selected_pairs(p.get()) };
  REQUIRE(selected.size() == 4);
  CHECK(selected == std::vector<std::string>{ "a", "b", "c", "d" });
}

TEST_CASE_FIXTURE(setup_test_fixture,
                  "unselected pairs stay unselected regardless of DEPENDS direction") {
  p->type = pkg_type::USER_MANAGED;
  p->setup_pairs.emplace("a", pkg::setup_pair_decl{});
  p->setup_pairs.emplace("b", pkg::setup_pair_decl{ .platforms = {}, .depends = { "a" } });
  p->setup_selected.insert("a");  // b depends on a, but selecting a must not pull b

  auto const selected{ compute_selected_pairs(p.get()) };
  REQUIRE(selected.size() == 1);
  CHECK(selected[0] == "a");
}

TEST_CASE_FIXTURE(setup_test_fixture, "selection marks the package consumed") {
  p->type = pkg_type::CACHE_MANAGED;
  p->setup_pairs.emplace("alpha", pkg::setup_pair_decl{});
  CHECK_FALSE(p->setup_selection_consumed);
  compute_selected_pairs(p.get());
  CHECK(p->setup_selection_consumed);
}

TEST_CASE_FIXTURE(setup_test_fixture, "unknown selected pair name throws") {
  p->type = pkg_type::CACHE_MANAGED;
  p->setup_pairs.emplace("alpha", pkg::setup_pair_decl{});
  p->setup_selected.insert("nonexistent");

  try {
    compute_selected_pairs(p.get());
    FAIL("Expected exception");
  } catch (std::runtime_error const &e) {
    std::string msg{ e.what() };
    CHECK(msg.find("nonexistent") != std::string::npos);
    CHECK(msg.find("test.package@v1") != std::string::npos);
  }
}

TEST_CASE_FIXTURE(setup_test_fixture,
                  "selection on spec with no SETUP pairs throws with clear message") {
  p->type = pkg_type::CACHE_MANAGED;
  p->setup_selected.insert("anything");

  try {
    compute_selected_pairs(p.get());
    FAIL("Expected exception");
  } catch (std::runtime_error const &e) {
    CHECK(std::string{ e.what() }.find("no SETUP pairs") != std::string::npos);
  }
}

}  // namespace envy
