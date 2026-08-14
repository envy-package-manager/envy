#include "phases/phase_spec_fetch.h"

#include "cache.h"
#include "engine.h"
#include "pkg.h"
#include "pkg_cfg.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <optional>

namespace envy {

namespace {

pkg_cfg *make_local_cfg(std::string identity,
                        std::filesystem::path const &file_path,
                        std::string serialized_options = "{}") {
  return pkg_cfg::pool()->emplace(std::move(identity),
                                  pkg_cfg::local_source{ file_path },
                                  std::move(serialized_options),
                                  std::nullopt,
                                  nullptr,
                                  nullptr,
                                  std::vector<pkg_cfg *>{},
                                  std::nullopt,
                                  file_path);
}

std::filesystem::path repo_path(std::string const &rel) {
  return std::filesystem::current_path() / rel;
}

struct temp_dir_guard {
  std::filesystem::path path;
  explicit temp_dir_guard(std::filesystem::path p) : path(std::move(p)) {}
  ~temp_dir_guard() { std::filesystem::remove_all(path); }
};

void expect_user_managed_cache_phase_error(std::string const &phase_name,
                                           std::string const &phase_rhs,
                                           std::string const &label_suffix = "") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  ("envy_test_check_" + phase_name + label_suffix) };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "spec.lua" };

  std::string const identity{ "test.check_" + phase_name + label_suffix + "@v1" };

  {
    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"" << identity << "\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n";
    ofs << phase_name << " = " << phase_rhs << "\n";
  }

  auto *cfg{ make_local_cfg(identity, spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  bool exception_thrown = false;
  std::string exception_msg;
  try {
    run_spec_fetch_phase(p, eng);
  } catch (std::runtime_error const &e) {
    exception_thrown = true;
    exception_msg = e.what();
  }

  std::filesystem::remove_all(temp_dir);

  REQUIRE(exception_thrown);
  CHECK(exception_msg.find("is user-managed (USER_MANAGED=true)") != std::string::npos);
  CHECK(exception_msg.find("declares " + phase_name) != std::string::npos);
}

}  // namespace

TEST_CASE("function check/install classified as user-managed") {
  cache c;
  engine eng{ c };

  auto *cfg{ make_local_cfg("local.brew@r0", repo_path("examples/local.brew@r0.lua")) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::USER_MANAGED);
}

TEST_CASE("string check/install classified as user-managed") {
  cache c;
  engine eng{ c };

  auto *cfg{ make_local_cfg("local.string_check_install@v1",
                            repo_path("test_data/specs/string_check_install.lua")) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::USER_MANAGED);
}

TEST_CASE("string check without install errors") {
  cache c;
  engine eng{ c };

  auto *cfg{ make_local_cfg("local.string_check_only@v1",
                            repo_path("test_data/specs/string_check_only.lua")) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS(run_spec_fetch_phase(p, eng));
}

// ============================================================================
// User-managed package validation tests (check verb + cache phases)
// ============================================================================

TEST_CASE("user-managed package with check verb and FETCH function throws parse error") {
  expect_user_managed_cache_phase_error(
      "FETCH",
      "function(install_dir, stage_dir, fetch_dir, tmp_dir) end",
      "_fn");
}

TEST_CASE("user-managed package with check verb and FETCH string throws parse error") {
  expect_user_managed_cache_phase_error("FETCH",
                                        "\"https://example.com/x.tar.gz\"",
                                        "_str");
}

TEST_CASE("user-managed package with check verb and FETCH table throws parse error") {
  expect_user_managed_cache_phase_error("FETCH",
                                        "{ url = \"https://example.com/x.tar.gz\" }",
                                        "_tbl");
}

TEST_CASE("user-managed package with check verb and STAGE function throws parse error") {
  expect_user_managed_cache_phase_error(
      "STAGE",
      "function(install_dir, stage_dir, fetch_dir, tmp_dir) end",
      "_fn");
}

TEST_CASE("user-managed package with check verb and STAGE string throws parse error") {
  expect_user_managed_cache_phase_error("STAGE", "\"echo stage\"", "_str");
}

TEST_CASE("user-managed package with check verb and STAGE table throws parse error") {
  expect_user_managed_cache_phase_error("STAGE", "{ strip_components = 1 }", "_tbl");
}

TEST_CASE("user-managed package with check verb and BUILD function throws parse error") {
  expect_user_managed_cache_phase_error(
      "BUILD",
      "function(install_dir, stage_dir, fetch_dir, tmp_dir) end",
      "_fn");
}

TEST_CASE("user-managed package with check verb and BUILD string throws parse error") {
  expect_user_managed_cache_phase_error("BUILD", "\"make\"", "_str");
}

TEST_CASE("user-managed package with top-level INSTALL throws parse error") {
  expect_user_managed_cache_phase_error("INSTALL", "\"echo install\"", "_str");
}

// ============================================================================
// SETUP schema validation tests
// ============================================================================

namespace {

// Write a spec file with the given body lines and run spec_fetch; returns the
// error message, or empty string on success.
std::string run_spec_body(std::string const &label, std::string const &body) {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  ("envy_test_setup_" + label) };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "spec.lua" };

  std::string const identity{ "test.setup_" + label + "@v1" };
  {
    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"" << identity << "\"\n" << body;
  }

  auto *cfg{ make_local_cfg(identity, spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  try {
    run_spec_fetch_phase(p, eng);
  } catch (std::runtime_error const &e) { return e.what(); }
  return {};
}

}  // namespace

TEST_CASE("top-level CHECK is rejected for cache-managed specs") {
  auto const msg{ run_spec_body("toplevel_check_cm",
                                "FETCH = \"https://example.com/x.tar.gz\"\n"
                                "CHECK = \"echo test\"\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("top-level CHECK") != std::string::npos);
  CHECK(msg.find("SETUP") != std::string::npos);
}

TEST_CASE("top-level CHECK is rejected regardless of value type") {
  auto const msg{ run_spec_body("toplevel_check_tbl",
                                "FETCH = \"https://example.com/x.tar.gz\"\n"
                                "CHECK = { shell = \"echo test\" }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("top-level CHECK") != std::string::npos);
}

TEST_CASE("top-level CHECK is rejected for user-managed specs") {
  auto const msg{ run_spec_body(
      "toplevel_check_um",
      "USER_MANAGED = true\n"
      "CHECK = \"echo test\"\n"
      "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x' } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("top-level CHECK") != std::string::npos);
}

TEST_CASE("user-managed spec without SETUP throws") {
  auto const msg{ run_spec_body("um_no_setup", "USER_MANAGED = true\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("at least one SETUP pair") != std::string::npos);
}

TEST_CASE("SETUP pair missing CHECK throws") {
  auto const msg{ run_spec_body("missing_check",
                                "USER_MANAGED = true\n"
                                "SETUP = { main = { INSTALL = 'echo x' } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("must define CHECK") != std::string::npos);
}

TEST_CASE("SETUP pair missing INSTALL throws") {
  auto const msg{ run_spec_body("missing_install",
                                "USER_MANAGED = true\n"
                                "SETUP = { main = { CHECK = 'exit 0' } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("must define INSTALL") != std::string::npos);
}

TEST_CASE("SETUP pair with unknown field throws") {
  auto const msg{ run_spec_body(
      "unknown_field",
      "USER_MANAGED = true\n"
      "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x', BOGUS = 1 } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("unknown field 'BOGUS'") != std::string::npos);
}

TEST_CASE("SETUP with non-table entry throws") {
  auto const msg{ run_spec_body("non_table_entry",
                                "USER_MANAGED = true\n"
                                "SETUP = { main = 'not a table' }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("must be a table") != std::string::npos);
}

TEST_CASE("SETUP with non-string key throws") {
  auto const msg{ run_spec_body(
      "non_string_key",
      "USER_MANAGED = true\n"
      "SETUP = { { CHECK = 'exit 0', INSTALL = 'echo x' } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("keys must be non-empty strings") != std::string::npos);
}

TEST_CASE("SETUP pair with bad PLATFORMS type throws") {
  auto const msg{ run_spec_body(
      "bad_platforms",
      "USER_MANAGED = true\n"
      "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x', PLATFORMS = 'linux' "
      "} }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("PLATFORMS must be a table") != std::string::npos);
}

TEST_CASE("cache-managed spec with SETUP parses pairs and platforms") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_setup_cm_pairs" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "spec.lua" };

  {
    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.setup_cm_pairs@v1\"\n";
    ofs << "FETCH = \"https://example.com/x.tar.gz\"\n";
    ofs << "SETUP = {\n";
    ofs << "  udev_rules = { CHECK = 'exit 0', INSTALL = 'echo x', PLATFORMS = { "
           "'linux' } },\n";
    ofs << "  extras = { CHECK = function(pkg_dir, opts) return true end, INSTALL = "
           "function(pkg_dir, opts) end },\n";
    ofs << "}\n";
  }

  auto *cfg{ make_local_cfg("test.setup_cm_pairs@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::CACHE_MANAGED);
  REQUIRE(p->setup_pairs.size() == 2);
  REQUIRE(p->setup_pairs.contains("udev_rules"));
  REQUIRE(p->setup_pairs.contains("extras"));
  REQUIRE(p->setup_pairs.at("udev_rules").platforms.size() == 1);
  CHECK(p->setup_pairs.at("udev_rules").platforms[0] == "linux");
  CHECK(p->setup_pairs.at("extras").platforms.empty());
}

TEST_CASE("SETUP pair DEPENDS parses into decl") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_setup_depends_parse" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "spec.lua" };

  {
    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.setup_depends_parse@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = {\n";
    ofs << "  a = { CHECK = 'exit 0', INSTALL = 'echo a' },\n";
    ofs << "  b = { CHECK = 'exit 0', INSTALL = 'echo b', DEPENDS = { 'a' } },\n";
    ofs << "}\n";
  }

  auto *cfg{ make_local_cfg("test.setup_depends_parse@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  REQUIRE(p->setup_pairs.at("b").depends.size() == 1);
  CHECK(p->setup_pairs.at("b").depends[0] == "a");
  CHECK(p->setup_pairs.at("a").depends.empty());
}

TEST_CASE("SETUP pair DEPENDS on unknown pair throws") {
  auto const msg{ run_spec_body(
      "depends_unknown",
      "USER_MANAGED = true\n"
      "SETUP = { a = { CHECK = 'exit 0', INSTALL = 'echo x', DEPENDS = { 'ghost' } } "
      "}\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("DEPENDS on unknown pair 'ghost'") != std::string::npos);
}

TEST_CASE("SETUP pair DEPENDS self-cycle throws") {
  auto const msg{ run_spec_body(
      "depends_self",
      "USER_MANAGED = true\n"
      "SETUP = { a = { CHECK = 'exit 0', INSTALL = 'echo x', DEPENDS = { 'a' } } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("SETUP DEPENDS cycle") != std::string::npos);
  CHECK(msg.find("a -> a") != std::string::npos);
}

TEST_CASE("SETUP pair DEPENDS cycle throws with path") {
  auto const msg{ run_spec_body(
      "depends_cycle",
      "USER_MANAGED = true\n"
      "SETUP = {\n"
      "  a = { CHECK = 'exit 0', INSTALL = 'echo x', DEPENDS = { 'b' } },\n"
      "  b = { CHECK = 'exit 0', INSTALL = 'echo x', DEPENDS = { 'c' } },\n"
      "  c = { CHECK = 'exit 0', INSTALL = 'echo x', DEPENDS = { 'a' } },\n"
      "}\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("SETUP DEPENDS cycle") != std::string::npos);
  CHECK(msg.find("->") != std::string::npos);
}

TEST_CASE("SETUP pair DEPENDS with non-table type throws") {
  auto const msg{ run_spec_body(
      "depends_bad_type",
      "USER_MANAGED = true\n"
      "SETUP = { a = { CHECK = 'exit 0', INSTALL = 'echo x', DEPENDS = 'b' } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("DEPENDS must be a table") != std::string::npos);
}

TEST_CASE("SETUP pair name with invalid characters throws") {
  auto const msg{ run_spec_body(
      "bad_pair_name",
      "USER_MANAGED = true\n"
      "SETUP = { ['bad name!'] = { CHECK = 'exit 0', INSTALL = 'echo x' } }\n") };
  REQUIRE(!msg.empty());
  CHECK(msg.find("invalid characters") != std::string::npos);
}

TEST_CASE("weak dependency entry with setup field carries selection onto record") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_dep_setup_weak" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };

  std::filesystem::path spec_file{ temp_dir / "spec.lua" };
  {
    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"local.dep_setup_weak@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x' } }\n";
    // No source => weak reference; selecting 'setup' is now allowed and the
    // selection rides on the weak_reference record until resolution.
    ofs << "DEPENDENCIES = { { spec = 'test.other@v1', setup = { 'foo', 'bar' } } }\n";
  }

  auto *cfg{ make_local_cfg("local.dep_setup_weak@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  REQUIRE(p->weak_references.size() == 1);
  auto const &wr{ p->weak_references[0] };
  CHECK(wr.query == "test.other@v1");
  REQUIRE(wr.setup.size() == 2);
  CHECK(wr.setup[0] == "foo");
  CHECK(wr.setup[1] == "bar");
}

TEST_CASE("weak product dependency with setup field carries selection onto record") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_dep_setup_weak_product" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };

  std::filesystem::path spec_file{ temp_dir / "spec.lua" };
  {
    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"local.dep_setup_weak_prod@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x' } }\n";
    // product ref with no source => weak product reference.
    ofs << "DEPENDENCIES = { { product = 'tool', setup = { 'main' } } }\n";
  }

  auto *cfg{ make_local_cfg("local.dep_setup_weak_prod@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  REQUIRE(p->weak_references.size() == 1);
  auto const &wr{ p->weak_references[0] };
  CHECK(wr.is_product);
  CHECK(wr.query == "tool");
  REQUIRE(wr.setup.size() == 1);
  CHECK(wr.setup[0] == "main");
}

TEST_CASE("strong dependency entry with setup field parses into dep cfg") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_dep_setup_ok" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };

  {
    std::ofstream ofs{ temp_dir / "other.lua" };
    ofs << "IDENTITY = \"local.other@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x' } }\n";
  }

  std::filesystem::path spec_file{ temp_dir / "spec.lua" };
  {
    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"local.dep_setup_ok@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x' } }\n";
    ofs << "DEPENDENCIES = { { spec = 'local.other@v1', source = 'other.lua', setup = "
           "{ 'main' } } }\n";
  }

  auto *cfg{ make_local_cfg("local.dep_setup_ok@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  REQUIRE(p->owned_dependency_cfgs.size() == 1);
  auto const *dep_cfg{ p->owned_dependency_cfgs[0] };
  REQUIRE(dep_cfg->setup.has_value());
  REQUIRE(dep_cfg->setup->size() == 1);
  CHECK((*dep_cfg->setup)[0] == "main");

  // ensure_pkg merged the dep entry's selection into the dep package
  pkg *dep{ eng.find_exact(pkg_key{ "local.other@v1" }) };
  REQUIRE(dep != nullptr);
  CHECK(dep->setup_selected.contains("main"));
}

TEST_CASE("user-managed package with check verb and install phase succeeds") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_check_install_ok" };
  std::filesystem::create_directories(temp_dir);
  std::filesystem::path spec_file{ temp_dir / "spec.lua" };

  std::ofstream ofs{ spec_file };
  ofs << "IDENTITY = \"test.check_install_ok@v1\"\n";
  ofs << "USER_MANAGED = true\n";
  ofs << "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n";
  ofs.close();

  auto *cfg{ make_local_cfg("test.check_install_ok@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  // Should not throw - install phase is allowed with check verb
  CHECK_NOTHROW(run_spec_fetch_phase(p, eng));

  std::filesystem::remove_all(temp_dir);
}

TEST_CASE("OPTIONS table succeeds and sees options") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_options_ok" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file_ok{ temp_dir / "options_ok.lua" };
  std::filesystem::path spec_file_true{ temp_dir / "options_true.lua" };
  {
    std::ofstream ofs{ spec_file_ok };
    ofs << "IDENTITY = \"test.options_ok@v1\"\n";
    ofs << "OPTIONS = { foo = {} }\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function(pkg_dir, options) return true "
           "end, INSTALL = function(pkg_dir, options) end } }\n";
    ofs.close();
  }

  spec_file_true.replace_filename("options_true.lua");
  {
    std::ofstream ofs{ spec_file_true };
    ofs << "IDENTITY = \"test.options_true@v1\"\n";
    ofs << "OPTIONS = function(opts) return true end\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function(pkg_dir, options) return true "
           "end, INSTALL = function(pkg_dir, options) end } }\n";
    ofs.close();
  }

  auto *cfg{ make_local_cfg("test.options_ok@v1", spec_file_ok, "{ foo = \"bar\" }") };
  pkg *p{ eng.ensure_pkg(cfg) };
  CHECK_NOTHROW(run_spec_fetch_phase(p, eng));

  auto *cfg_true{ make_local_cfg("test.options_true@v1", spec_file_true) };
  pkg *p_true{ eng.ensure_pkg(cfg_true) };
  CHECK_NOTHROW(run_spec_fetch_phase(p_true, eng));
}

TEST_CASE("OPTIONS function returns false fails") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_options_false" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "options_false.lua" };

  std::ofstream ofs{ spec_file };
  ofs << "IDENTITY = \"test.options_false@v1\"\n";
  ofs << "OPTIONS = function(opts) return false end\n";
  ofs << "USER_MANAGED = true\n";
  ofs << "SETUP = { main = { CHECK = function(pkg_dir, options) return true "
         "end, INSTALL = function(pkg_dir, options) end } }\n";
  ofs.close();

  auto *cfg{ make_local_cfg("test.options_false@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  bool threw{ false };
  try {
    run_spec_fetch_phase(p, eng);
  } catch (std::runtime_error const &e) {
    threw = true;
    std::string const msg{ e.what() };
    INFO(msg);
    CHECK(msg.find("returned false") != std::string::npos);
  }

  CHECK(threw);
}

TEST_CASE("OPTIONS function returns string fails with message") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_options_string" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "options_string.lua" };

  std::ofstream ofs{ spec_file };
  ofs << "IDENTITY = \"test.options_string@v1\"\n";
  ofs << "OPTIONS = function(opts) return \"nope\" end\n";
  ofs << "USER_MANAGED = true\n";
  ofs << "SETUP = { main = { CHECK = function(pkg_dir, options) return true "
         "end, INSTALL = function(pkg_dir, options) end } }\n";
  ofs.close();

  auto *cfg{ make_local_cfg("test.options_string@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  bool threw{ false };
  try {
    run_spec_fetch_phase(p, eng);
  } catch (std::runtime_error const &e) {
    threw = true;
    std::string const msg{ e.what() };
    INFO(msg);
    CHECK(msg.find("nope") != std::string::npos);
  }
  CHECK(threw);
}

TEST_CASE("OPTIONS function invalid return type errors") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_options_type" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "options_type.lua" };

  std::ofstream ofs{ spec_file };
  ofs << "IDENTITY = \"test.options_type@v1\"\n";
  ofs << "OPTIONS = function(opts) return 123 end\n";
  ofs << "USER_MANAGED = true\n";
  ofs << "SETUP = { main = { CHECK = function(pkg_dir, options) return true "
         "end, INSTALL = function(pkg_dir, options) end } }\n";
  ofs.close();

  auto *cfg{ make_local_cfg("test.options_type@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS(run_spec_fetch_phase(p, eng));
}

TEST_CASE("OPTIONS set to non-table non-function errors") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_options_nonfn" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "options_nonfn.lua" };

  std::ofstream ofs{ spec_file };
  ofs << "IDENTITY = \"test.options_nonfn@v1\"\n";
  ofs << "OPTIONS = 42\n";
  ofs << "USER_MANAGED = true\n";
  ofs << "SETUP = { main = { CHECK = function(pkg_dir, options) return true "
         "end, INSTALL = function(pkg_dir, options) end } }\n";
  ofs.close();

  auto *cfg{ make_local_cfg("test.options_nonfn@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS(run_spec_fetch_phase(p, eng));
}

TEST_CASE("OPTIONS function runtime error bubbles with context") {
  cache c;
  engine eng{ c };

  std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                  "envy_test_options_error" };
  std::filesystem::create_directories(temp_dir);
  temp_dir_guard guard{ temp_dir };
  std::filesystem::path spec_file{ temp_dir / "options_error.lua" };

  std::ofstream ofs{ spec_file };
  ofs << "IDENTITY = \"test.options_error@v1\"\n";
  ofs << "OPTIONS = function(opts) error(\"boom\") end\n";
  ofs << "USER_MANAGED = true\n";
  ofs << "SETUP = { main = { CHECK = function(pkg_dir, options) return true "
         "end, INSTALL = function(pkg_dir, options) end } }\n";
  ofs.close();

  auto *cfg{ make_local_cfg("test.options_error@v1", spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  bool threw{ false };
  try {
    run_spec_fetch_phase(p, eng);
  } catch (std::runtime_error const &e) {
    threw = true;
    std::string const msg{ e.what() };
    INFO(msg);
    CHECK(msg.find("Lua error in test.options_error@v1") != std::string::npos);
    CHECK(msg.find("boom") != std::string::npos);
  }
  CHECK(threw);
}

TEST_CASE("product name validation") {
  auto expect_valid{ [](std::string const &product_name) {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_product_valid" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.product@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { [\"" << product_name << "\"] = \"/usr/bin/tool\" }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.product@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CAPTURE(product_name);
    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.count(product_name) == 1);
  } };

  auto expect_rejected{ [](std::string const &lua_escaped_name) {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_product_reject" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.product@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { [\"" << lua_escaped_name << "\"] = \"/usr/bin/tool\" }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.product@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CAPTURE(lua_escaped_name);
    CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng), doctest::Contains("shell-unsafe"));
  } };

  SUBCASE("valid") {
    expect_valid("tool");
    expect_valid("Tool123");
    expect_valid("my-tool");
    expect_valid("my_tool");
    expect_valid("arm-none-eabi-gcc");
    expect_valid("python3.14");
    expect_valid("g++");
    expect_valid("arm-none-eabi-c++");
    expect_valid("tool@v1");
    expect_valid("tool:variant");
    expect_valid("tool#tag");
    expect_valid("tool~beta");
    expect_valid("tool[0]");
  }

  SUBCASE("invalid") {
    expect_rejected("my tool");
    expect_rejected("tool$var");
    expect_rejected("tool`cmd`");
    expect_rejected("tool\\\"name");
    expect_rejected("tool'name");
    expect_rejected("tool%var%");
    expect_rejected("tool\\\\path");
    expect_rejected("tool!");
  }
}

TEST_CASE("PRODUCTS table syntax") {
  SUBCASE("string value produces script=true") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_string" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = \"/usr/bin/tool\" }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.count("tool") == 1);
    CHECK(p->products.at("tool").value == "/usr/bin/tool");
    CHECK(p->products.at("tool").script == true);
  }

  SUBCASE("table value with script=true") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_table_true" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = { value = \"/usr/bin/tool\", script = true } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.count("tool") == 1);
    CHECK(p->products.at("tool").value == "/usr/bin/tool");
    CHECK(p->products.at("tool").script == true);
  }

  SUBCASE("table value with script=false") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_table_false" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { lib = { value = \"/usr/lib/libfoo.so\", script = false } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.count("lib") == 1);
    CHECK(p->products.at("lib").value == "/usr/lib/libfoo.so");
    CHECK(p->products.at("lib").script == false);
  }

  SUBCASE("table value without script defaults to true") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_table_default" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = { value = \"/usr/bin/tool\" } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.count("tool") == 1);
    CHECK(p->products.at("tool").value == "/usr/bin/tool");
    CHECK(p->products.at("tool").script == true);
  }

  SUBCASE("table missing value field throws") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_missing_value" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { lib = { script = false } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                      doctest::Contains("must have string 'value' field"));
  }

  SUBCASE("table with invalid value type throws") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_invalid_value" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { lib = { value = 123 } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                      doctest::Contains("must have string 'value' field"));
  }

  SUBCASE("mixed string and table forms") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_mixed" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = {\n";
    ofs << "  tool = \"/usr/bin/tool\",\n";
    ofs << "  lib = { value = \"/usr/lib/libfoo.so\", script = false },\n";
    ofs << "}\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.count("tool") == 1);
    CHECK(p->products.at("tool").value == "/usr/bin/tool");
    CHECK(p->products.at("tool").script == true);
    CHECK(p->products.count("lib") == 1);
    CHECK(p->products.at("lib").value == "/usr/lib/libfoo.so");
    CHECK(p->products.at("lib").script == false);
  }

  SUBCASE("table value with platforms field") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_platforms" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = { value = \"/usr/bin/tool\", "
           "platforms = {\"darwin\", \"linux\"} } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.count("tool") == 1);
    CHECK(p->products.at("tool").value == "/usr/bin/tool");
    CHECK(p->products.at("tool").script == true);
    REQUIRE(p->products.at("tool").platforms.size() == 2);
    CHECK(p->products.at("tool").platforms[0] == "darwin");
    CHECK(p->products.at("tool").platforms[1] == "linux");
  }

  SUBCASE("table value without platforms defaults to empty") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_no_platforms" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = { value = \"/usr/bin/tool\" } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.at("tool").platforms.empty());
  }

  SUBCASE("string value has empty platforms") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_string_platforms" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = \"/usr/bin/tool\" }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.at("tool").platforms.empty());
  }

  SUBCASE("invalid platforms type throws") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_bad_plat_type" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = { value = \"/usr/bin/tool\", "
           "platforms = \"darwin\" } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                      doctest::Contains("platforms must be a table"));
  }

  SUBCASE("invalid platforms entry type throws") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_bad_plat_entry" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = { value = \"/usr/bin/tool\", "
           "platforms = {123} } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                      doctest::Contains("platforms entries must be strings"));
  }

  SUBCASE("empty platforms entry throws") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_empty_plat_entry" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = { tool = { value = \"/usr/bin/tool\", "
           "platforms = {\"\"} } }\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                      doctest::Contains("platforms entry cannot be empty"));
  }

  SUBCASE("mixed products with and without platforms") {
    cache c;
    engine eng{ c };

    std::filesystem::path temp_dir{ std::filesystem::temp_directory_path() /
                                    "envy_test_products_mixed_plats" };
    std::filesystem::create_directories(temp_dir);
    temp_dir_guard guard{ temp_dir };
    std::filesystem::path spec_file{ temp_dir / "spec.lua" };

    std::ofstream ofs{ spec_file };
    ofs << "IDENTITY = \"test.products@v1\"\n";
    ofs << "USER_MANAGED = true\n";
    ofs << "SETUP = { main = { CHECK = function() return true end, INSTALL = function() "
           "end } }\n";
    ofs << "PRODUCTS = {\n";
    ofs << "  tool = \"/usr/bin/tool\",\n";
    ofs << "  completer = { value = \"/usr/bin/completer\", "
           "platforms = {\"darwin\", \"linux\"} },\n";
    ofs << "  lib = { value = \"/usr/lib/libfoo.so\", script = false },\n";
    ofs << "}\n";
    ofs.close();

    auto *cfg{ make_local_cfg("test.products@v1", spec_file) };
    pkg *p{ eng.ensure_pkg(cfg) };

    CHECK_NOTHROW(run_spec_fetch_phase(p, eng));
    CHECK(p->products.at("tool").platforms.empty());
    REQUIRE(p->products.at("completer").platforms.size() == 2);
    CHECK(p->products.at("completer").platforms[0] == "darwin");
    CHECK(p->products.at("completer").platforms[1] == "linux");
    CHECK(p->products.at("lib").platforms.empty());
  }
}

// ============================================================================
// USER_MANAGED top-level declaration tests
// ============================================================================

namespace {

struct user_managed_fixture {
  std::filesystem::path temp_dir;
  std::filesystem::path spec_file;

  explicit user_managed_fixture(std::string const &label) {
    temp_dir =
        std::filesystem::temp_directory_path() / ("envy_test_user_managed_" + label);
    std::filesystem::create_directories(temp_dir);
    spec_file = temp_dir / "spec.lua";
  }

  ~user_managed_fixture() { std::filesystem::remove_all(temp_dir); }

  void write(std::string const &body) {
    std::ofstream ofs{ spec_file };
    ofs << body;
  }
};

}  // namespace

TEST_CASE("USER_MANAGED=true with SETUP pair classifies user-managed") {
  user_managed_fixture f{ "true_ok" };
  f.write(
      "IDENTITY = \"test.um_true_ok@v1\"\n"
      "USER_MANAGED = true\n"
      "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_true_ok@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::USER_MANAGED);
}

TEST_CASE("USER_MANAGED=false with FETCH classifies cache-managed") {
  user_managed_fixture f{ "false_fetch" };
  f.write(
      "IDENTITY = \"test.um_false_fetch@v1\"\n"
      "USER_MANAGED = false\n"
      "FETCH = function() end\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_false_fetch@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::CACHE_MANAGED);
}

TEST_CASE("USER_MANAGED absent with FETCH defaults to cache-managed") {
  user_managed_fixture f{ "absent_fetch" };
  f.write(
      "IDENTITY = \"test.um_absent@v1\"\n"
      "FETCH = function() end\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_absent@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::CACHE_MANAGED);
}

TEST_CASE("USER_MANAGED=true without SETUP throws") {
  user_managed_fixture f{ "true_no_check" };
  f.write(
      "IDENTITY = \"test.um_true_no_check@v1\"\n"
      "USER_MANAGED = true\n"
      "INSTALL = \"echo install\"\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_true_no_check@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                    doctest::Contains("at least one SETUP pair"));
}

TEST_CASE("USER_MANAGED=true with top-level CHECK throws") {
  user_managed_fixture f{ "true_no_install" };
  f.write(
      "IDENTITY = \"test.um_true_no_install@v1\"\n"
      "USER_MANAGED = true\n"
      "CHECK = \"echo test\"\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_true_no_install@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                    doctest::Contains("defines top-level CHECK"));
}

TEST_CASE("USER_MANAGED=false with CHECK throws (forbidden combination)") {
  user_managed_fixture f{ "false_with_check" };
  f.write(
      "IDENTITY = \"test.um_false_with_check@v1\"\n"
      "USER_MANAGED = false\n"
      "FETCH = function() end\n"
      "CHECK = \"echo test\"\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_false_with_check@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                    doctest::Contains("defines top-level CHECK"));
}

TEST_CASE("USER_MANAGED absent with CHECK throws") {
  user_managed_fixture f{ "absent_with_check" };
  f.write(
      "IDENTITY = \"test.um_absent_check@v1\"\n"
      "CHECK = \"echo test\"\n"
      "INSTALL = \"echo install\"\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_absent_check@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                    doctest::Contains("defines top-level CHECK"));
}

TEST_CASE("USER_MANAGED function returning true classifies user-managed") {
  user_managed_fixture f{ "fn_true" };
  f.write(
      "IDENTITY = \"test.um_fn_true@v1\"\n"
      "USER_MANAGED = function() return true end\n"
      "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_fn_true@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::USER_MANAGED);
}

TEST_CASE("USER_MANAGED function returning false classifies cache-managed") {
  user_managed_fixture f{ "fn_false" };
  f.write(
      "IDENTITY = \"test.um_fn_false@v1\"\n"
      "USER_MANAGED = function() return false end\n"
      "FETCH = function() end\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_fn_false@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  REQUIRE_NOTHROW(run_spec_fetch_phase(p, eng));
  CHECK(p->type == pkg_type::CACHE_MANAGED);
}

TEST_CASE("USER_MANAGED function returning non-boolean throws") {
  user_managed_fixture f{ "fn_nonbool" };
  f.write(
      "IDENTITY = \"test.um_fn_nonbool@v1\"\n"
      "USER_MANAGED = function() return \"yes\" end\n"
      "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_fn_nonbool@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                    doctest::Contains("must return a boolean"));
}

TEST_CASE("USER_MANAGED function raising error throws with identity") {
  user_managed_fixture f{ "fn_error" };
  f.write(
      "IDENTITY = \"test.um_fn_error@v1\"\n"
      "USER_MANAGED = function() error(\"boom\") end\n"
      "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_fn_error@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  bool threw{ false };
  try {
    run_spec_fetch_phase(p, eng);
  } catch (std::runtime_error const &e) {
    threw = true;
    std::string const msg{ e.what() };
    INFO(msg);
    CHECK(msg.find("test.um_fn_error@v1") != std::string::npos);
    CHECK(msg.find("boom") != std::string::npos);
  }
  CHECK(threw);
}

TEST_CASE("USER_MANAGED with non-boolean non-function type throws") {
  user_managed_fixture f{ "bad_type" };
  f.write(
      "IDENTITY = \"test.um_bad_type@v1\"\n"
      "USER_MANAGED = \"yes\"\n"
      "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_bad_type@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng),
                    doctest::Contains("must be a boolean or function"));
}

// ============================================================================
// Cache-entry finalization
//
// A cache entry carrying `envy-complete` is trusted forever after -- nothing
// revalidates its contents. So a fetch that cannot produce a usable artifact must
// never finalize its entry, or one bad fetch becomes a permanent failure that no
// amount of repairing the source can clear. These cover the bundle path; the spec
// paths (remote/git/custom fetch) share the single finalize point in
// run_spec_fetch_phase and are covered by functional_tests/test_cache_poisoning.py.
// ============================================================================

namespace {

pkg_cfg *make_bundle_cfg(std::string const &identity,
                         std::filesystem::path const &bundle_dir) {
  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(
      identity,
      pkg_cfg::bundle_source{ identity, pkg_cfg::local_source{ bundle_dir } },
      std::string{ "{}" },
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::nullopt,
      bundle_dir) };
  cfg->bundle_identity = identity;  // identity == bundle identity => BUNDLE_ONLY
  return cfg;
}

struct bundle_entry_fixture {
  std::filesystem::path root;
  std::filesystem::path cache_root;
  std::filesystem::path bundle_dir;
  std::string identity;

  explicit bundle_entry_fixture(std::string const &label)
      : root{ std::filesystem::temp_directory_path() / ("envy_test_bundle_entry_" + label) },
        cache_root{ root / "cache" },
        bundle_dir{ root / "src" },
        identity{ "test.bundle-" + label + "@v1" } {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(bundle_dir / "specs");
  }

  ~bundle_entry_fixture() { std::filesystem::remove_all(root); }

  // Rewrites the bundle source in place. An empty spec_body writes no spec file,
  // which is what SPECS-points-at-nothing validation failure needs.
  void write_source(std::string const &manifest, std::string const &spec_body) {
    std::filesystem::remove(bundle_dir / "specs" / "a.lua");
    std::ofstream{ bundle_dir / "envy-bundle.lua" } << manifest;
    if (!spec_body.empty()) {
      std::ofstream{ bundle_dir / "specs" / "a.lua" } << spec_body;
    }
  }

  std::string valid_manifest() const {
    return "BUNDLE = \"" + identity +
           "\"\nSPECS = { [\"test.a@v1\"] = \"specs/a.lua\" }\n";
  }

  static std::string valid_spec() {
    return "IDENTITY = \"test.a@v1\"\n"
           "USER_MANAGED = true\n"
           "SETUP = { main = { CHECK = 'exit 0', INSTALL = 'echo x' } }\n";
  }

  // Entries live at specs/<identity>/<source-key>/; a fixture declares exactly one
  // source, so at most one such directory exists.
  std::optional<std::filesystem::path> entry_dir() const {
    std::filesystem::path const parent{ cache_root / "specs" / identity };
    if (std::filesystem::is_directory(parent)) {
      for (auto const &e : std::filesystem::directory_iterator{ parent }) {
        if (e.is_directory()) { return e.path(); }
      }
    }
    return std::nullopt;
  }

  bool entry_complete() const {
    auto const dir{ entry_dir() };
    return dir && cache::is_entry_complete(*dir);
  }

  bool entry_has(std::string const &relative) const {
    auto const dir{ entry_dir() };
    return dir && std::filesystem::exists(*dir / relative);
  }

  // One materialization attempt against the on-disk cache, in its own engine and
  // cache objects -- the stand-in for a fresh envy process. Returns the error
  // message, or an empty string on success.
  std::string materialize() {
    cache c{ cache_root };
    engine eng{ c };
    pkg *p{ eng.ensure_pkg(make_bundle_cfg(identity, bundle_dir)) };
    try {
      run_spec_fetch_phase(p, eng);
    } catch (std::exception const &e) { return e.what(); }
    return {};
  }
};

}  // namespace

TEST_CASE("valid bundle finalizes its cache entry") {
  bundle_entry_fixture f{ "ok" };
  f.write_source(f.valid_manifest(), bundle_entry_fixture::valid_spec());

  CHECK(f.materialize().empty());
  CHECK(f.entry_complete());
  CHECK(f.entry_has("pkg/envy-bundle.lua"));
}

TEST_CASE("unparseable bundle manifest leaves the cache entry unfinalized") {
  bundle_entry_fixture f{ "parse" };
  f.write_source("BUNDLE = \"" + f.identity + "\"\nthis is not lua(((\n",
                 bundle_entry_fixture::valid_spec());

  auto const msg{ f.materialize() };
  REQUIRE(!msg.empty());
  CHECK(msg.find("Failed to parse bundle manifest") != std::string::npos);
  CHECK_FALSE(f.entry_complete());
  CHECK_FALSE(f.entry_has("pkg"));
}

TEST_CASE("bundle identity mismatch leaves the cache entry unfinalized") {
  bundle_entry_fixture f{ "identity" };
  f.write_source("BUNDLE = \"test.someone-else@v9\"\n"
                 "SPECS = { [\"test.a@v1\"] = \"specs/a.lua\" }\n",
                 bundle_entry_fixture::valid_spec());

  auto const msg{ f.materialize() };
  REQUIRE(!msg.empty());
  CHECK(msg.find("Bundle identity mismatch") != std::string::npos);
  CHECK_FALSE(f.entry_complete());
}

TEST_CASE("bundle whose spec file is missing leaves the cache entry unfinalized") {
  bundle_entry_fixture f{ "validate" };
  f.write_source(f.valid_manifest(), "");  // SPECS points at a file that is not there

  auto const msg{ f.materialize() };
  REQUIRE(!msg.empty());
  CHECK(msg.find("file not found") != std::string::npos);
  CHECK_FALSE(f.entry_complete());
}

TEST_CASE("repairing a bad bundle source clears the failure on the next run") {
  bundle_entry_fixture f{ "repair" };
  f.write_source("BUNDLE = \"" + f.identity + "\"\nbroken((\n",
                 bundle_entry_fixture::valid_spec());

  REQUIRE(!f.materialize().empty());
  REQUIRE_FALSE(f.entry_complete());

  // Same failure on a second run: the entry is still cold, not a cached error.
  REQUIRE(!f.materialize().empty());
  REQUIRE_FALSE(f.entry_complete());

  f.write_source(f.valid_manifest(), bundle_entry_fixture::valid_spec());

  CHECK(f.materialize().empty());
  CHECK(f.entry_complete());
}

TEST_CASE("USER_MANAGED=true with FETCH throws (cache-managed phase forbidden)") {
  user_managed_fixture f{ "true_with_fetch" };
  f.write(
      "IDENTITY = \"test.um_true_fetch@v1\"\n"
      "USER_MANAGED = true\n"
      "SETUP = { main = { CHECK = \"echo test\", INSTALL = \"echo install\" } }\n"
      "FETCH = function() end\n");

  cache c;
  engine eng{ c };
  auto *cfg{ make_local_cfg("test.um_true_fetch@v1", f.spec_file) };
  pkg *p{ eng.ensure_pkg(cfg) };

  CHECK_THROWS_WITH(run_spec_fetch_phase(p, eng), doctest::Contains("declares FETCH"));
}

}  // namespace envy
