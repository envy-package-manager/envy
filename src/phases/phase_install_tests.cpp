#include "phase_install.h"

#include "cache.h"
#include "engine.h"
#include "lua_envy.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "sol_util.h"

#include "doctest.h"

#include <sol/sol.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

namespace envy {

namespace {

// Fixture for testing install phase with temporary cache
struct install_test_fixture {
  pkg_cfg *cfg;
  std::unique_ptr<pkg> p;
  std::filesystem::path temp_root;
  cache test_cache;
  engine eng;

  install_test_fixture()
      : temp_root{ []() {
          static std::mt19937_64 rng{ std::random_device{}() };
          auto suffix = std::to_string(rng());
          return std::filesystem::temp_directory_path() /
                 std::filesystem::path("envy-install-test-" + suffix);
        }() },
        test_cache{ temp_root },
        eng{ test_cache } {
    // Create temp cache directory
    std::filesystem::create_directories(temp_root);

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
    auto lua_state{ sol_util_make_lua_state() };
    lua_envy_install(*lua_state);

    // Initialize options to empty table
    p = std::unique_ptr<pkg>(new pkg{ .key = pkg_key(*cfg),
                                      .cfg = cfg,
                                      .cache_ptr = &test_cache,
                                      .eng = nullptr,
                                      .lua = std::move(lua_state),
                                      .lock = nullptr,
                                      .canonical_identity_hash = {},
                                      .pkg_path = std::filesystem::path{},
                                      .spec_file_path = std::nullopt,
                                      .result_hash = {},
                                      .type = pkg_type::CACHE_MANAGED,
                                      .declared_dependencies = {},
                                      .owned_dependency_cfgs = {},
                                      .dependencies = {},
                                      .product_dependencies = {},
                                      .weak_references = {},
                                      .products = {},
                                      .resolved_weak_dependency_keys = {} });
  }

  ~install_test_fixture() {
    p.reset();
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
  }

  // Returns the guard accessor (holds the lua mutex). The temporary lives to the end
  // of the full expression it's used in, so `(*lua())[key] = v` runs under the lock;
  // bind it to a named local when driving the state across multiple statements.
  sol_state_guard::accessor lua() { return p->lua.lock(); }

  void clear_install_verb() { (*lua())["INSTALL"] = sol::lua_nil; }

  void set_install_string(std::string_view install_code) {
    (*lua())["INSTALL"] = std::string(install_code);
  }

  void set_install_function(std::string_view install_code) {
    auto const acc{ lua() };
    sol::state_view state{ *acc };
    std::string code = "INSTALL = " + std::string(install_code);
    sol::protected_function_result result =
        state.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
      sol::error err = result;
      throw std::runtime_error(std::string("Failed to set install function: ") +
                               err.what());
    }
  }

  void acquire_lock() {
    auto result = test_cache.ensure_pkg("test.package@v1", "darwin", "arm64", "deadbeef");
    REQUIRE(result.lock != nullptr);
    p->lock = std::move(result.lock);
  }

  void write_install_content() {
    REQUIRE(p->lock);
    auto install_file = p->lock->install_dir() / "output.txt";
    std::ofstream ofs{ install_file };
    ofs << "installed";
  }
};

}  // namespace

// ============================================================================
// Basic install phase behavior
// ============================================================================

TEST_CASE_FIXTURE(install_test_fixture,
                  "install phase auto-marks package complete on success") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      -- Auto-marked complete on successful return
    end
  )");

  acquire_lock();
  write_install_content();

  CHECK_NOTHROW(run_install_phase(p.get(), eng));

  // Verify pkg was marked complete
  CHECK(p->pkg_path.string().find("pkg") != std::string::npos);
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function receives all directory arguments as strings") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      if type(install_dir) ~= "string" then
        error("expected install_dir to be string")
      end
      if type(stage_dir) ~= "string" then
        error("expected stage_dir to be string")
      end
      if type(fetch_dir) ~= "string" then
        error("expected fetch_dir to be string")
      end
      if type(tmp_dir) ~= "string" then
        error("expected tmp_dir to be string")
      end
    end
  )");

  acquire_lock();
  write_install_content();

  CHECK_NOTHROW(run_install_phase(p.get(), eng));
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install phase string install succeeds and marks complete") {
  set_install_string("echo 'installing'");

  acquire_lock();

  CHECK_NOTHROW(run_install_phase(p.get(), eng));

  // Verify pkg_path was set (marked complete)
  CHECK(!p->pkg_path.empty());
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install phase string install with non-zero exit throws") {
  set_install_string("exit 1");

  acquire_lock();

  bool exception_thrown = false;
  std::string exception_msg;
  try {
    run_install_phase(p.get(), eng);
  } catch (std::runtime_error const &e) {
    exception_thrown = true;
    exception_msg = e.what();
  }
  REQUIRE(exception_thrown);
  CHECK(exception_msg.find("Install shell script failed") != std::string::npos);
  CHECK(exception_msg.find("exit code 1") != std::string::npos);
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install phase with no lock skips work (cache hit)") {
  // Don't acquire lock (simulates cache hit)
  p->lock = nullptr;

  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      error("Should not run - no lock means cache hit")
    end
  )");

  // Should not throw or run install function
  CHECK_NOTHROW(run_install_phase(p.get(), eng));
}

// ============================================================================
// Install function return type validation tests
// ============================================================================

TEST_CASE_FIXTURE(install_test_fixture, "install function returning nil succeeds") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return nil
    end
  )");

  acquire_lock();
  CHECK_NOTHROW(run_install_phase(p.get(), eng));
}

TEST_CASE_FIXTURE(install_test_fixture, "install function with no return succeeds") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      -- No return statement
    end
  )");

  acquire_lock();
  CHECK_NOTHROW(run_install_phase(p.get(), eng));
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function returning number throws with type error") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return 42
    end
  )");

  acquire_lock();

  bool exception_thrown = false;
  std::string exception_msg;
  try {
    run_install_phase(p.get(), eng);
  } catch (std::runtime_error const &e) {
    exception_thrown = true;
    exception_msg = e.what();
  }
  REQUIRE(exception_thrown);
  CHECK(exception_msg.find("must return nil or string") != std::string::npos);
  CHECK(exception_msg.find("number") != std::string::npos);
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function returning table throws with type error") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return {foo = "bar"}
    end
  )");

  acquire_lock();

  bool exception_thrown = false;
  std::string exception_msg;
  try {
    run_install_phase(p.get(), eng);
  } catch (std::runtime_error const &e) {
    exception_thrown = true;
    exception_msg = e.what();
  }
  REQUIRE(exception_thrown);
  CHECK(exception_msg.find("must return nil or string") != std::string::npos);
  CHECK(exception_msg.find("table") != std::string::npos);
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function returning boolean throws with type error") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return true
    end
  )");

  acquire_lock();

  bool exception_thrown = false;
  std::string exception_msg;
  try {
    run_install_phase(p.get(), eng);
  } catch (std::runtime_error const &e) {
    exception_thrown = true;
    exception_msg = e.what();
  }
  REQUIRE(exception_thrown);
  CHECK(exception_msg.find("must return nil or string") != std::string::npos);
  CHECK(exception_msg.find("boolean") != std::string::npos);
}

// ============================================================================
// Install function returning string tests
// ============================================================================

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function returning string executes shell and marks complete") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return "echo 'returned string executed'"
    end
  )");

  acquire_lock();

  CHECK_NOTHROW(run_install_phase(p.get(), eng));

  // Verify pkg_path was set (indicates marked complete)
  CHECK(!p->pkg_path.empty());
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function can call envy.run and return string") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      envy.run("echo 'first command'")
      return "echo 'second command'"
    end
  )");

  acquire_lock();

  // Both envy.run() and returned string should execute
  CHECK_NOTHROW(run_install_phase(p.get(), eng));
  CHECK(!p->pkg_path.empty());
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function returning string with non-zero exit throws") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return "exit 1"
    end
  )");

  acquire_lock();

  bool exception_thrown = false;
  std::string exception_msg;
  try {
    run_install_phase(p.get(), eng);
  } catch (std::runtime_error const &e) {
    exception_thrown = true;
    exception_msg = e.what();
  }
  REQUIRE(exception_thrown);
  CHECK(exception_msg.find("Install shell script failed") != std::string::npos);
  CHECK(exception_msg.find("exit code 1") != std::string::npos);
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "install function returning empty string succeeds") {
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return ""
    end
  )");

  acquire_lock();

  // Empty string should succeed (no-op shell script)
  CHECK_NOTHROW(run_install_phase(p.get(), eng));
  CHECK(!p->pkg_path.empty());
}

// ============================================================================
// Install phase cwd tests (cache-managed packages use stage_dir as cwd)
// ============================================================================

TEST_CASE_FIXTURE(install_test_fixture, "string install cwd is stage_dir") {
  acquire_lock();

  auto const expected_cwd{ std::filesystem::canonical(p->lock->stage_dir()).string() };
  auto const cwd_file{ temp_root / "string_install_cwd.txt" };
#ifdef _WIN32
  set_install_string("[IO.File]::WriteAllText('" + cwd_file.string() + "', $PWD.Path)");
#else
  set_install_string("pwd -P > " + cwd_file.string());
#endif

  CHECK_NOTHROW(run_install_phase(p.get(), eng));

  REQUIRE(std::filesystem::exists(cwd_file));
  std::ifstream ifs{ cwd_file };
  std::string cwd_output;
  std::getline(ifs, cwd_output);
  CHECK(cwd_output == expected_cwd);
}

TEST_CASE_FIXTURE(install_test_fixture, "function install envy.run cwd is stage_dir") {
  acquire_lock();

  auto const expected_cwd{ std::filesystem::canonical(p->lock->stage_dir()).string() };
  auto const cwd_file{ temp_root / "func_install_cwd.txt" };
  (*lua())["CWD_FILE"] = cwd_file.string();

#ifdef _WIN32
  set_install_function(R"lua(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      envy.run("[IO.File]::WriteAllText('" .. CWD_FILE .. "', $PWD.Path)")
    end
  )lua");
#else
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      envy.run("pwd -P > " .. CWD_FILE)
    end
  )");
#endif

  CHECK_NOTHROW(run_install_phase(p.get(), eng));

  REQUIRE(std::filesystem::exists(cwd_file));
  std::ifstream ifs{ cwd_file };
  std::string cwd_output;
  std::getline(ifs, cwd_output);
  CHECK(cwd_output == expected_cwd);
}

TEST_CASE_FIXTURE(install_test_fixture,
                  "function install returning string cwd is stage_dir") {
  acquire_lock();

  auto const expected_cwd{ std::filesystem::canonical(p->lock->stage_dir()).string() };
  auto const cwd_file{ temp_root / "returned_string_cwd.txt" };
  (*lua())["CWD_FILE"] = cwd_file.string();

#ifdef _WIN32
  set_install_function(R"lua(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return "[IO.File]::WriteAllText('" .. CWD_FILE .. "', $PWD.Path)"
    end
  )lua");
#else
  set_install_function(R"(
    function(install_dir, stage_dir, fetch_dir, tmp_dir)
      return "pwd -P > " .. CWD_FILE
    end
  )");
#endif

  CHECK_NOTHROW(run_install_phase(p.get(), eng));

  REQUIRE(std::filesystem::exists(cwd_file));
  std::ifstream ifs{ cwd_file };
  std::string cwd_output;
  std::getline(ifs, cwd_output);
  CHECK(cwd_output == expected_cwd);
}

}  // namespace envy
